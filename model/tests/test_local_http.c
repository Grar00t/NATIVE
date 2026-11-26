#include "../Core_CPP/casper_rag.c"
#include <sys/wait.h>

static int framing(char *wire, const char *expected) {
    unsigned code = 0;
    HBuf *b = parse_local_http(wire, strlen(wire), &code);
    int ok = expected ? (b && code == 200 && !strcmp(b->buf, expected)) : b == NULL;
    hbuf_free(b);
    return ok;
}

int main(void) {
    char fixed[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
    char chunks[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n1;ext=yes\r\nc\r\n0\r\nX-Final: ok\r\n\r\n";
    char short_body[] = "HTTP/1.0 200 OK\r\nContent-Length: 9\r\n\r\nabc";
    char both[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    char incomplete[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nab";
    if (!framing(fixed, "abc") || !framing(chunks, "abc") || !framing(short_body, NULL) ||
        !framing(both, NULL) || !framing(incomplete, NULL)) return 1;
    int server = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (server < 0 || bind(server, (struct sockaddr *)&a, sizeof a) || listen(server, 1) ||
        getsockname(server, (struct sockaddr *)&a, &len)) return 2;
    pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        alarm(5);
        int conn = accept(server, NULL, NULL);
        char request[2048] = {0};
        size_t used = 0;
        while (conn >= 0 && used < sizeof request - 1 && !strstr(request, "\r\n\r\n")) {
            ssize_t n = read(conn, request + used, sizeof request - 1 - used);
            if (n <= 0) _exit(3);
            used += (size_t)n;
        }
        if (!strstr(request, "GET /search?q=abc HTTP/1.0\r\n")) _exit(3);
        static const char response[] = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok";
        if (write(conn, response, sizeof response - 1) != sizeof response - 1) _exit(4);
        close(conn); close(server); _exit(0);
    }
    close(server);
    char host[32];
    snprintf(host, sizeof host, "127.0.0.1:%u", (unsigned)ntohs(a.sin_port));
    unsigned code = 0;
    HBuf *result = http_get(host, "/search?q=abc", &code);
    int ok = result && code == 200 && !strcmp(result->buf, "ok");
    hbuf_free(result);
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) || !ok) return 5;
    puts("loopback HTTP: fixed length, chunked, malformed input and live socket passed");
    return 0;
}
