#define _POSIX_C_SOURCE 200809L

#include "niyah_dalw_json.h"
#include "../native/niyah_utf8.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NIYAH_JSON_MAX_K 1000u

typedef struct {
    char *p;
    size_t cap;
    size_t len;
    int bad;
} Buf;

static void put(Buf *b, const char *s, size_t n)
{
    if (b->bad) return;
    if (n > (size_t)INT_MAX - b->len || (b->p && n >= b->cap - b->len)) {
        b->bad = 1;
        return;
    }
    if (b->p) {
        memcpy(b->p + b->len, s, n);
        b->p[b->len + n] = '\0';
    }
    b->len += n;
}

static void put_str(Buf *b, const char *s)
{
    put(b, s, strlen(s));
}

static void put_quoted(Buf *b, const char *s)
{
    char t[8];
    put(b, "\"", 1u);
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        size_t start = i;
        uint32_t c = niyah_utf8_next((const unsigned char *)s, n, &i);
        if (c == '"' || c == '\\') {
            t[0] = '\\';
            t[1] = (char)c;
            put(b, t, 2u);
        } else if (c < 0x20u) {
            (void)snprintf(t, sizeof t, "\\u%04x", (unsigned)c);
            put(b, t, 6u);
        } else if (c == 0xFFFD && i - start == 1) {
            put(b, "?", 1u);
        } else {
            put(b, s + start, i - start);
        }
    }
    put(b, "\"", 1u);
}

static void set_status(int *status, int value)
{
    if (status) *status = value;
}

int niyah_dalw_search_json(const NiyahDalw *handle, const char *query, int exact,
                           size_t k, char *out, size_t cap, int *status)
{
    NiyahDalwHit *hits;
    char tmp[512];
    Buf b;
    size_t n, i;
    int st = NIYAH_DALW_OK;

    set_status(status, NIYAH_DALW_OK);
    if (out && cap) out[0] = '\0';
    if (!handle || !query || (out == NULL) != (cap == 0u) || k == 0u) {
        set_status(status, NIYAH_DALW_E_ARG);
        return -1;
    }
    if (k > NIYAH_JSON_MAX_K) k = NIYAH_JSON_MAX_K;
    hits = (NiyahDalwHit *)malloc(k * sizeof *hits);
    if (!hits) {
        set_status(status, NIYAH_DALW_E_MEM);
        return -1;
    }
    n = niyah_dalw_query(handle, query, exact, hits, k, &st);
    if (st != NIYAH_DALW_OK) {
        free(hits);
        set_status(status, st);
        return -1;
    }

    b.p = out;
    b.cap = cap;
    b.len = 0u;
    b.bad = 0;
    put_str(&b, "{\"query\":");
    put_quoted(&b, query);
    (void)snprintf(tmp, sizeof tmp, ",\"exact\":%s,\"count\":%lu,\"hits\":[",
                   exact ? "true" : "false", (unsigned long)n);
    put_str(&b, tmp);
    for (i = 0u; i < n; i++) {
        int rl;
        (void)snprintf(tmp, sizeof tmp,
                       "%s{\"rank\":%lu,\"document_id\":\"%016llx\",\"score\":%.6f,"
                       "\"shard\":%lu,\"doc\":%lu,\"receipt\":",
                       i ? "," : "", (unsigned long)(i + 1u),
                       (unsigned long long)hits[i].document_id, (double)hits[i].score,
                       (unsigned long)hits[i].shard, (unsigned long)hits[i].doc);
        put_str(&b, tmp);
        if (b.bad) break;
        rl = niyah_dalw_receipt(handle, &hits[i], query, exact,
                                out ? out + b.len : NULL, out ? cap - b.len : 0u);
        if (rl < 0) {
            st = out && niyah_dalw_receipt(handle, &hits[i], query, exact, NULL, 0u) >= 0
                     ? NIYAH_DALW_E_MEM : NIYAH_DALW_E_CORRUPT;
            break;
        }
        if ((size_t)rl > (size_t)INT_MAX - b.len) {
            st = NIYAH_DALW_E_MEM;
            break;
        }
        b.len += (size_t)rl;
        put_str(&b, "}");
    }
    put_str(&b, "]}");
    free(hits);
    if (b.bad || st != NIYAH_DALW_OK) {
        if (out) out[0] = '\0';
        set_status(status, st != NIYAH_DALW_OK ? st : NIYAH_DALW_E_MEM);
        return -1;
    }
    return (int)b.len;
}
