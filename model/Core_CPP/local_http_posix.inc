#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

static int wait_socket(int fd, short events, uint32_t start) {
    struct pollfd p = {fd, events, 0};
    for (;;) {
        uint32_t elapsed = ms_now() - start;
        if (elapsed >= RAG_TIMEOUT_MS) return 0;
        int rc = poll(&p, 1, (int)(RAG_TIMEOUT_MS - elapsed));
        if (rc < 0 && errno == EINTR) continue;
        return rc > 0 && !(p.revents & POLLNVAL);
    }
}

static int decimal_length(const char *s, size_t *out) {
    size_t n = 0;
    if (!*s) return 0;
    while (*s) {
        unsigned d = (unsigned char)*s++ - (unsigned)'0';
        if (d > 9 || n > (SIZE_MAX - d) / 10) return 0;
        n = n * 10 + d;
    }
    *out = n;
    return 1;
}

static int append_chunks(HBuf *out, char *p, char *end) {
    while (p < end) {
        char *line = strstr(p, "\r\n");
        if (!line || line - p > 1024) return 0;
        size_t n = 0;
        char *digit = p;
        for (; p < line && *p != ';'; ++p) {
            int c = tolower((unsigned char)*p);
            unsigned d;
            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else return 0;
            if (n > (HBUF_MAX - d) / 16) return 0;
            n = n * 16 + d;
        }
        if (digit == p) return 0;
        p = line + 2;
        if (!n) {
            while (p < end) {
                line = strstr(p, "\r\n");
                if (!line) return 0;
                if (line == p) return line + 2 == end;
                if (!memchr(p, ':', (size_t)(line - p))) return 0;
                p = line + 2;
            }
            return 0;
        }
        if ((size_t)(end - p) < n + 2 || memcmp(p + n, "\r\n", 2)) return 0;
        if (hbuf_app(out, p, n)) return 0;
        p += n + 2;
    }
    return 0;
}

static HBuf *parse_local_http(char *raw, size_t n, unsigned *status) {
    if (memchr(raw, 0, n)) return NULL;
    raw[n] = 0;
    char *body = strstr(raw, "\r\n\r\n");
    char *line = strstr(raw, "\r\n");
    if (!body || !line || body - raw > 16384 || line - raw < 12) return NULL;
    if (strncmp(raw, "HTTP/1.0 ", 9) && strncmp(raw, "HTTP/1.1 ", 9)) return NULL;
    if (raw[9] < '1' || raw[9] > '5' || !isdigit((unsigned char)raw[10]) ||
        !isdigit((unsigned char)raw[11]) || (line != raw+12 && raw[12] != ' ')) return NULL;
    unsigned code = (unsigned)((raw[9]-'0')*100 + (raw[10]-'0')*10 + raw[11]-'0');
    int chunked = 0, have_length = 0;
    size_t length = 0;
    for (char *h = line + 2; h < body; h = line + 2) {
        line = strstr(h, "\r\n");
        char *colon = memchr(h, ':', (size_t)(line - h));
        if (!colon || colon == h) return NULL;
        *colon++ = 0;
        while (colon < line && (*colon == ' ' || *colon == '\t')) ++colon;
        char *tail = line;
        while (tail > colon && (tail[-1] == ' ' || tail[-1] == '\t')) --tail;
        *tail = 0;
        if (!strcasecmp(h, "Content-Length")) {
            if (have_length || !decimal_length(colon, &length)) return NULL;
            have_length = 1;
        } else if (!strcasecmp(h, "Transfer-Encoding")) {
            if (chunked || strcasecmp(colon, "chunked")) return NULL;
            chunked = 1;
        }
    }
    body += 4;
    size_t available = (size_t)((raw + n) - body);
    if ((chunked && have_length) || (have_length && length != available)) return NULL;
    HBuf *out = hbuf_new();
    if (!out) return NULL;
    int ok = chunked ? append_chunks(out, body, raw+n) : hbuf_app(out, body, available) == 0;
    if (!ok || !out->len) { hbuf_free(out); return NULL; }
    if (status) *status = code;
    return out;
}

static HBuf *http_get(const char *host, const char *path, unsigned *status) {
    const unsigned short port = local_endpoint_port(host);
    if (status) *status = 0;
    if (!port || !path || strncmp(path, "/search?", 8)) return NULL;
    for (const unsigned char *q = (const unsigned char *)path; *q; ++q)
        if (*q <= 32 || *q >= 127) return NULL;
    char request[2048];
    int size = snprintf(request, sizeof request,
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\nAccept: application/json,text/html\r\n\r\n", path, host);
    if (size < 0 || (size_t)size >= sizeof request) return NULL;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    HBuf *out = NULL;
    char *raw = NULL;
    uint32_t start = ms_now();
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) goto done;
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) < 0) goto done;
#endif
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0 && errno != EINPROGRESS) goto done;
    if (!wait_socket(fd, POLLOUT, start)) goto done;
    int error = 0;
    socklen_t error_size = sizeof error;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) < 0 || error) goto done;
    for (size_t sent = 0; sent < (size_t)size;) {
        if (!wait_socket(fd, POLLOUT, start)) goto done;
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t n = send(fd, request+sent, (size_t)size-sent, flags);
        if (n < 0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if (n <= 0) goto done;
        sent += (size_t)n;
    }
    size_t used = 0, cap = HBUF_MAX + 16384u;
    raw = malloc(cap+1);
    if (!raw) goto done;
    while (used < cap) {
        if (!wait_socket(fd, POLLIN, start)) goto done;
        ssize_t n = recv(fd, raw+used, cap-used, 0);
        if (n < 0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if (n < 0) goto done;
        if (n == 0) { out = parse_local_http(raw, used, status); break; }
        used += (size_t)n;
    }
done:
    free(raw);
    close(fd);
    return out;
}
