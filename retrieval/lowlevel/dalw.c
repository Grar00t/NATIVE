/* dalw.c v2: tokenizer + Arabic light stemmer, posting lists (+tf), shard builder/reader,
   AND query, BM25 top-k, receipts. C11, little-endian. */
#define _POSIX_C_SOURCE 200809L
#include "dalw.h"
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <wchar.h>
static wchar_t *wide_path(const char *utf8) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (length <= 0 || (size_t)length > SIZE_MAX / sizeof(wchar_t)) return NULL;
    wchar_t *wide = malloc((size_t)length * sizeof *wide);
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, length)) {
        free(wide);
        return NULL;
    }
    return wide;
}
static int create_shard(const char *path) {
    wchar_t *wide = wide_path(path);
    if (!wide) return -1;
    int fd = _wopen(wide, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, 0600);
    free(wide);
    return fd;
}
static void discard_shard(const char *path) {
    wchar_t *wide = wide_path(path);
    if (wide) { _wremove(wide); free(wide); }
}
static FILE *read_shard(const char *path) {
    wchar_t *wide = wide_path(path);
    if (!wide) return NULL;
    FILE *file = _wfopen(wide, L"rb");
    free(wide);
    return file;
}
#endif
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define DALW_MMAP 1
#endif
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "dalw needs a little-endian machine"
#endif

#define MAGIC "DALW2\0\0\0"
#define FNV_OFF 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL
#define STEM_SALT 0x5354454D53414C54ULL
#define SKIP_CP 0xFFFFFFFFu
#define HDR 64u
#define TERM_SZ 32u
#define DOC_SZ 64u
#define CAP_MAX 160u
#define MAXTOK 64
#define STR_MAX 0xFFFFFFu
#define BM25_K1 1.2f
#define BM25_B 0.75f

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static int in_range(uint64_t off, uint64_t len, uint64_t limit) { return off <= limit && len <= limit - off; }

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33; return x;
}

/* ================= UTF-8 + normalisation ================= */

static uint32_t utf8_next(const unsigned char *s, size_t len, size_t *i) {
    unsigned c = s[*i]; size_t n = len - *i;
    if (c < 0x80) { (*i)++; return c; }
    if (c >= 0xC2 && c <= 0xDF && n >= 2 && (s[*i+1] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x1Fu) << 6) | (s[*i+1] & 0x3Fu); *i += 2; return cp;
    }
    if (c >= 0xE0 && c <= 0xEF && n >= 3 && (s[*i+1] & 0xC0) == 0x80 && (s[*i+2] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x0Fu) << 12) | ((s[*i+1] & 0x3Fu) << 6) | (s[*i+2] & 0x3Fu);
        if (cp >= 0x800) { *i += 3; return cp; }
    }
    if (c >= 0xF0 && c <= 0xF4 && n >= 4 && (s[*i+1] & 0xC0) == 0x80 &&
        (s[*i+2] & 0xC0) == 0x80 && (s[*i+3] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x07u) << 18) | ((s[*i+1] & 0x3Fu) << 12) |
                      ((s[*i+2] & 0x3Fu) << 6) | (s[*i+3] & 0x3Fu);
        if (cp >= 0x10000 && cp <= 0x10FFFF) { *i += 4; return cp; }
    }
    (*i)++; return 0xFFFD;
}

/* normalised code point, 0 = separator, SKIP_CP = ignore (diacritic/tatweel) */
static uint32_t norm_cp(uint32_t cp) {
    if (cp < 0x80) {
        if (cp >= 'a' && cp <= 'z') return cp;
        if (cp >= '0' && cp <= '9') return cp;
        if (cp >= 'A' && cp <= 'Z') return cp + 32;
        return 0;
    }
    if (cp == 0xFEFF || cp == 0xFFFD) return 0;
    if (cp < 0xC0) return 0;
    if (cp < 0x250) {
        if (cp == 0xD7 || cp == 0xF7) return 0;
        if (cp >= 0xC0 && cp <= 0xDE) return cp + 32;
        return cp;
    }
    if (cp >= 0x600 && cp <= 0x6FF) {
        if (cp >= 0x660 && cp <= 0x669) return '0' + (cp - 0x660);
        if (cp >= 0x6F0 && cp <= 0x6F9) return '0' + (cp - 0x6F0);
        if (cp == 0x640 || (cp >= 0x64B && cp <= 0x65F) || cp == 0x670) return SKIP_CP;
        if (cp == 0x622 || cp == 0x623 || cp == 0x625 || cp == 0x671) return 0x627;
        if (cp == 0x649) return 0x64A;
        if (cp == 0x629) return 0x647;
        if (cp <= 0x605 || cp == 0x60C || cp == 0x61B || cp == 0x61C || cp == 0x61F ||
            (cp >= 0x66A && cp <= 0x66D) || cp == 0x6D4 || cp == 0x6DD) return 0;
        return cp;
    }
    if ((cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x3000 && cp <= 0x303F)) return 0;
    return cp;
}

static size_t put_utf8(uint32_t cp, unsigned char *o) {
    if (cp < 0x80) { o[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) { o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 0x3F); o[2] = 0x80 | (cp & 0x3F); return 3; }
    o[0] = 0xF0 | (cp >> 18); o[1] = 0x80 | ((cp >> 12) & 0x3F);
    o[2] = 0x80 | ((cp >> 6) & 0x3F); o[3] = 0x80 | (cp & 0x3F); return 4;
}

/* ================= tokenizer ================= */

typedef void (*CpCb)(const uint32_t *cps, size_t n, void *u);

static void tokenize_cps(const char *text, size_t len, CpCb cb, void *u) {
    const unsigned char *s = (const unsigned char *)text;
    uint32_t buf[MAXTOK]; size_t i = 0, n = 0;
    if (!text || !cb) return;
    while (i < len) {
        uint32_t c = norm_cp(utf8_next(s, len, &i));
        if (c == SKIP_CP) continue;
        if (c == 0) { if (n) { cb(buf, n, u); n = 0; } continue; }
        if (n < MAXTOK) buf[n++] = c;
    }
    if (n) cb(buf, n, u);
}

static uint64_t hash_cps(const uint32_t *c, size_t n) {
    uint64_t h = FNV_OFF; unsigned char b[4];
    for (size_t i = 0; i < n; i++) {
        size_t m = put_utf8(c[i], b);
        for (size_t k = 0; k < m; k++) h = (h ^ b[k]) * FNV_PRIME;
    }
    return mix64(h);
}

/* ---- Arabic light stemmer (rule based; a lexicon-backed FST can replace this later) ---- */

typedef struct { const uint32_t *p; size_t n, minrest; } Aff;
static const uint32_t P_WAL[] = {0x648, 0x627, 0x644}, P_BAL[] = {0x628, 0x627, 0x644},
                      P_KAL[] = {0x643, 0x627, 0x644}, P_FAL[] = {0x641, 0x627, 0x644},
                      P_LL[] = {0x644, 0x644}, P_AL[] = {0x627, 0x644}, P_W[] = {0x648};
static const Aff PRE[] = {{P_WAL,3,3},{P_BAL,3,3},{P_KAL,3,3},{P_FAL,3,3},{P_LL,2,3},{P_AL,2,3},{P_W,1,4}};
static const uint32_t S_HA[] = {0x647, 0x627}, S_AN[] = {0x627, 0x646}, S_AT[] = {0x627, 0x62A},
                      S_WN[] = {0x648, 0x646}, S_YN[] = {0x64A, 0x646}, S_YH[] = {0x64A, 0x647},
                      S_H[] = {0x647}, S_Y[] = {0x64A};
static const Aff SUF[] = {{S_HA,2,3},{S_AN,2,3},{S_AT,2,3},{S_WN,2,3},{S_YN,2,3},{S_YH,2,3},{S_H,1,3},{S_Y,1,3}};

static int cps_eq(const uint32_t *a, const uint32_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int is_arabic_tok(const uint32_t *c, size_t n) { return n && c[0] >= 0x600 && c[0] <= 0x6FF; }

static size_t stem_cps(const uint32_t *in, size_t n, uint32_t *out) {
    size_t b = 0, e = n;
    for (size_t i = 0; i < sizeof PRE / sizeof PRE[0]; i++)
        if (e - b >= PRE[i].n + PRE[i].minrest && cps_eq(in + b, PRE[i].p, PRE[i].n)) { b += PRE[i].n; break; }
    for (size_t i = 0; i < sizeof SUF / sizeof SUF[0]; i++)
        if (e - b >= SUF[i].n + SUF[i].minrest && cps_eq(in + e - SUF[i].n, SUF[i].p, SUF[i].n)) { e -= SUF[i].n; break; }
    memcpy(out, in + b, (e - b) * sizeof(uint32_t));
    return e - b;
}
static uint64_t stem_hash(const uint32_t *c, size_t n) {
    uint32_t st[MAXTOK]; size_t m = stem_cps(c, n, st);
    return mix64(hash_cps(st, m) ^ STEM_SALT);
}

/* ---- public tokenizer API ---- */

typedef struct { DalwTokFn fn; void *user; } SurfCtx;
static void surf_cb(const uint32_t *c, size_t n, void *u) { SurfCtx *x = (SurfCtx *)u; x->fn(hash_cps(c, n), x->user); }
void dalw_tokens(const char *text, size_t len, DalwTokFn fn, void *user) {
    SurfCtx x = {fn, user};
    if (fn) tokenize_cps(text, len, surf_cb, &x);
}

typedef struct { uint64_t h; int got; } FirstCtx;
static void first_cb(const uint32_t *c, size_t n, void *u) {
    FirstCtx *x = (FirstCtx *)u;
    if (!x->got) { x->h = hash_cps(c, n); x->got = 1; }
}
uint64_t dalw_term_hash(const char *word) {
    FirstCtx x = {0, 0};
    if (word) tokenize_cps(word, strlen(word), first_cb, &x);
    return x.h;
}

typedef struct { uint64_t *out; size_t n, max; int mode; } QCtx;
static void q_cb(const uint32_t *c, size_t n, void *u) {
    QCtx *x = (QCtx *)u;
    uint64_t h = (x->mode == DALW_Q_STEM && is_arabic_tok(c, n)) ? stem_hash(c, n) : hash_cps(c, n);
    for (size_t i = 0; i < x->n; i++) if (x->out[i] == h) return;
    if (x->n < x->max) x->out[x->n++] = h;
}
size_t dalw_parse_query(const char *text, int mode, uint64_t *out, size_t max) {
    QCtx x = {out, 0, max, mode};
    if (text && out) tokenize_cps(text, strlen(text), q_cb, &x);
    return x.n;
}

typedef struct { char *out; size_t cap, len; int got; } StemCtx;
static void stem_cb(const uint32_t *c, size_t n, void *u) {
    StemCtx *x = (StemCtx *)u; uint32_t st[MAXTOK]; unsigned char b[4];
    if (x->got) return;
    x->got = 1;
    size_t m = is_arabic_tok(c, n) ? stem_cps(c, n, st) : (memcpy(st, c, n * 4), n);
    for (size_t i = 0; i < m; i++) {
        size_t k = put_utf8(st[i], b);
        if (x->len + k + 1 > x->cap) return;
        memcpy(x->out + x->len, b, k); x->len += k;
    }
}
size_t dalw_stem_utf8(const char *word, char *out, size_t cap) {
    StemCtx x = {out, cap, 0, 0};
    if (!out || !cap) return 0;
    out[0] = 0;
    if (word) tokenize_cps(word, strlen(word), stem_cb, &x);
    out[x.len] = 0; return x.len;
}

/* ================= posting lists ================= */

size_t dalw_plist_bound(uint32_t n) {
    size_t nb = ((size_t)n + 127) / 128;
    return nb * 9 + (size_t)n * 4 + 16;
}

size_t dalw_plist_encode(const uint32_t *docs, uint32_t n, uint8_t *out) {
    if (n == 0) return 0;
    for (uint32_t i = 1; i < n; i++) if (docs[i] <= docs[i-1]) return 0;
    size_t nb = ((size_t)n + 127) / 128, pos = nb * 8;
    for (size_t b = 0; b < nb; b++) {
        const uint32_t *d = docs + b * 128;
        uint32_t cnt = (uint32_t)(n - b * 128 < 128 ? n - b * 128 : 128), mx = 0;
        for (uint32_t i = 1; i < cnt; i++) { uint32_t v = d[i] - d[i-1] - 1; if (v > mx) mx = v; }
        unsigned w = 0; while (w < 32 && (mx >> w)) w++;
        wr32(out + b * 8, d[0]); wr32(out + b * 8 + 4, (uint32_t)pos);
        out[pos++] = (uint8_t)w;
        uint64_t acc = 0; unsigned nbits = 0;
        for (uint32_t i = 1; i < cnt; i++) {
            acc |= (uint64_t)(d[i] - d[i-1] - 1) << nbits; nbits += w;
            while (nbits >= 8) { out[pos++] = (uint8_t)acc; acc >>= 8; nbits -= 8; }
        }
        if (nbits) out[pos++] = (uint8_t)acc;
    }
    return pos;
}

int dalw_plist_decode(const uint8_t *p, size_t bytes, uint32_t df, uint32_t *out) {
    if (df == 0) return 0;
    size_t nb = ((size_t)df + 127) / 128;
    if (bytes < nb * 8) return -1;
    uint32_t last = 0;
    for (size_t b = 0; b < nb; b++) {
        uint32_t first = rd32(p + b * 8); size_t off = rd32(p + b * 8 + 4);
        uint32_t cnt = (uint32_t)(df - b * 128 < 128 ? df - b * 128 : 128);
        if (off < nb * 8 || off >= bytes) return -1;
        unsigned w = p[off++]; if (w > 32) return -1;
        size_t need = (((size_t)(cnt - 1)) * w + 7) / 8;
        if (need > bytes - off) return -1;
        if (b && first <= last) return -1;
        uint32_t *o = out + b * 128; o[0] = first;
        uint64_t acc = 0, prev = first; unsigned nbits = 0;
        uint64_t mask = w == 32 ? 0xFFFFFFFFULL : ((1ULL << w) - 1);
        for (uint32_t i = 1; i < cnt; i++) {
            while (nbits < w) { acc |= (uint64_t)p[off++] << nbits; nbits += 8; }
            uint64_t v = acc & mask; acc >>= w; nbits -= w;
            prev += v + 1; if (prev > 0xFFFFFFFFULL) return -1;
            o[i] = (uint32_t)prev;
        }
        last = o[cnt - 1];
    }
    return 0;
}

/* tf stream: per 128-block: width byte + packed (tf-1) */
size_t dalw_tf_encode(const uint32_t *tf, uint32_t n, uint8_t *out) {
    if (n == 0) return 0;
    size_t pos = 0, nb = ((size_t)n + 127) / 128;
    for (size_t b = 0; b < nb; b++) {
        const uint32_t *t = tf + b * 128;
        uint32_t cnt = (uint32_t)(n - b * 128 < 128 ? n - b * 128 : 128), mx = 0;
        for (uint32_t i = 0; i < cnt; i++) { if (t[i] == 0) return 0; if (t[i] - 1 > mx) mx = t[i] - 1; }
        unsigned w = 0; while (w < 32 && (mx >> w)) w++;
        out[pos++] = (uint8_t)w;
        uint64_t acc = 0; unsigned nbits = 0;
        for (uint32_t i = 0; i < cnt; i++) {
            acc |= (uint64_t)(t[i] - 1) << nbits; nbits += w;
            while (nbits >= 8) { out[pos++] = (uint8_t)acc; acc >>= 8; nbits -= 8; }
        }
        if (nbits) out[pos++] = (uint8_t)acc;
    }
    return pos;
}

int dalw_tf_decode(const uint8_t *p, size_t bytes, uint32_t n, uint32_t *out) {
    size_t pos = 0, nb = ((size_t)n + 127) / 128;
    for (size_t b = 0; b < nb; b++) {
        uint32_t cnt = (uint32_t)(n - b * 128 < 128 ? n - b * 128 : 128);
        if (pos >= bytes) return -1;
        unsigned w = p[pos++]; if (w > 32) return -1;
        size_t need = ((size_t)cnt * w + 7) / 8;
        if (need > bytes - pos) return -1;
        uint64_t acc = 0, mask = w == 32 ? 0xFFFFFFFFULL : ((1ULL << w) - 1); unsigned nbits = 0;
        uint32_t *o = out + b * 128;
        for (uint32_t i = 0; i < cnt; i++) {
            while (nbits < w) { acc |= (uint64_t)p[pos++] << nbits; nbits += 8; }
            uint64_t v = acc & mask; acc >>= w; nbits -= w;
            if (v == 0xFFFFFFFFULL) return -1;
            o[i] = (uint32_t)v + 1;
        }
    }
    return 0;
}

/* ================= builder ================= */

typedef struct { uint64_t term; uint32_t doc; } Pair;
typedef struct {
    uint64_t fp, woff;
    char *title, *cap, *url, *wfile, *digest;
    uint32_t title_len, cap_len, url_len, wfile_len, digest_len, ntok, wlen;
} BDoc;
struct DalwBuilder { Pair *pairs; size_t np, cp; BDoc *docs; size_t nd, cd; };
typedef struct { uint8_t *p; size_t n, c; } Buf;

static int buf_put(Buf *b, const void *d, size_t n) {
    if (n > SIZE_MAX - b->n) return -1;
    if (b->n + n > b->c) {
        size_t c = b->c ? b->c : 4096;
        while (c < b->n + n) { if (c > SIZE_MAX / 2) return -1; c *= 2; }
        uint8_t *q = (uint8_t *)realloc(b->p, c); if (!q) return -1;
        b->p = q; b->c = c;
    }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n; return 0;
}

DalwBuilder *dalw_builder_new(void) { return (DalwBuilder *)calloc(1, sizeof(DalwBuilder)); }

static void bdoc_free(BDoc *d) { free(d->title); free(d->cap); free(d->url); free(d->wfile); free(d->digest); memset(d, 0, sizeof *d); }

void dalw_builder_free(DalwBuilder *b) {
    if (!b) return;
    for (size_t i = 0; i < b->nd; i++) bdoc_free(&b->docs[i]);
    free(b->docs); free(b->pairs); free(b);
}

typedef struct { DalwBuilder *b; uint32_t doc; uint64_t fp; uint32_t ntok; int fail; } AddCtx;
static void push_pair(AddCtx *c, uint64_t h) {
    DalwBuilder *b = c->b;
    if (c->fail) return;
    if (b->np == b->cp) {
        size_t nc = b->cp ? b->cp * 2 : 1024;
        if (nc > SIZE_MAX / sizeof(Pair)) { c->fail = 1; return; }
        Pair *q = (Pair *)realloc(b->pairs, nc * sizeof(Pair));
        if (!q) { c->fail = 1; return; }
        b->pairs = q; b->cp = nc;
    }
    b->pairs[b->np].term = h; b->pairs[b->np].doc = c->doc; b->np++;
}
static void add_cb(const uint32_t *c, size_t n, void *u) {
    AddCtx *x = (AddCtx *)u; uint64_t h = hash_cps(c, n);
    push_pair(x, h); x->fp = (x->fp ^ h) * FNV_PRIME; x->ntok++;
    if (is_arabic_tok(c, n)) push_pair(x, stem_hash(c, n));
}

static char *dup_n(const char *s, size_t n) {
    char *d = (char *)malloc(n + 1); if (!d) return NULL;
    if (n) memcpy(d, s, n);
    d[n] = 0; return d;
}
static int dup_str(const char *s, char **dst, uint32_t *len) {
    size_t n = s ? strlen(s) : 0;
    if (n > STR_MAX) return -1;
    *dst = dup_n(s ? s : "", n); *len = (uint32_t)n;
    return *dst ? 0 : -1;
}

int dalw_builder_add_ex(DalwBuilder *b, const DalwSrc *src) {
    if (!b || !src || (!src->text && src->text_len)) return -1;
    const char *text = src->text ? src->text : ""; size_t len = src->text ? src->text_len : 0;
    if (b->nd >= 0xFFFFFFF0u) return -1;
    if (b->nd == b->cd) {
        size_t nc = b->cd ? b->cd * 2 : 64;
        BDoc *q = (BDoc *)realloc(b->docs, nc * sizeof(BDoc)); if (!q) return -1;
        b->docs = q; b->cd = nc;
    }
    BDoc *d = &b->docs[b->nd]; memset(d, 0, sizeof *d);
    size_t cut = len < CAP_MAX ? len : CAP_MAX;
    while (cut > 0 && cut < len && (((unsigned char)text[cut]) & 0xC0) == 0x80) cut--;
    if (dup_str(src->title, &d->title, &d->title_len) || dup_str(src->url, &d->url, &d->url_len) ||
        dup_str(src->warc_file, &d->wfile, &d->wfile_len) || dup_str(src->digest, &d->digest, &d->digest_len)) {
        bdoc_free(d); return -1;
    }
    d->cap = dup_n(text, cut); d->cap_len = (uint32_t)cut;
    if (!d->cap) { bdoc_free(d); return -1; }
    d->woff = src->warc_offset; d->wlen = src->warc_len;
    size_t np0 = b->np;
    AddCtx c = {b, (uint32_t)b->nd, FNV_OFF, 0, 0};
    tokenize_cps(text, len, add_cb, &c);
    if (c.fail) { b->np = np0; bdoc_free(d); return -1; }
    d->fp = mix64(c.fp); d->ntok = c.ntok;
    b->nd++; return 0;
}

int dalw_builder_add(DalwBuilder *b, const char *title, const char *text, size_t len) {
    DalwSrc s; memset(&s, 0, sizeof s);
    s.title = title; s.text = text; s.text_len = len;
    return dalw_builder_add_ex(b, &s);
}

static int pair_cmp(const void *x, const void *y) {
    const Pair *a = (const Pair *)x, *b = (const Pair *)y;
    if (a->term != b->term) return a->term < b->term ? -1 : 1;
    return a->doc < b->doc ? -1 : a->doc > b->doc;
}

static int heap_str(Buf *heap, const char *s, uint32_t len, uint8_t *off_len) {
    if (heap->n > 0xFFFFFFFFu - (uint64_t)len) return -1;
    wr32(off_len, (uint32_t)heap->n); wr32(off_len + 4, len);
    return buf_put(heap, s, len);
}

int dalw_builder_write(DalwBuilder *b, const char *path) {
    if (!b || !path) return -1;
    int rc = -1;
    Buf terms = {0}, post = {0}, docs = {0}, heap = {0}, file = {0};
    uint32_t *tmp = NULL, *tf = NULL; uint8_t *enc = NULL;
    uint32_t nterms = 0; uint64_t total_tokens = 0;
    if (b->np) qsort(b->pairs, b->np, sizeof(Pair), pair_cmp);
    tmp = (uint32_t *)malloc((b->nd + 1) * sizeof(uint32_t));
    tf = (uint32_t *)malloc((b->nd + 1) * sizeof(uint32_t));
    enc = (uint8_t *)malloc(dalw_plist_bound((uint32_t)b->nd) + 16);
    if (!tmp || !tf || !enc) goto done;
    for (size_t i = 0; i < b->np;) {
        size_t j = i; uint32_t df = 0;
        while (j < b->np && b->pairs[j].term == b->pairs[i].term) {
            uint32_t doc = b->pairs[j].doc;
            if (df == 0 || tmp[df-1] != doc) { tmp[df] = doc; tf[df] = 1; df++; } else tf[df-1]++;
            j++;
        }
        uint8_t e[TERM_SZ] = {0};
        wr64(e, b->pairs[i].term); wr64(e + 8, post.n); wr32(e + 16, df);
        size_t db = dalw_plist_encode(tmp, df, enc);
        if (!db || db > 0xFFFFFFFFu || buf_put(&post, enc, db)) goto done;
        size_t tb = dalw_tf_encode(tf, df, enc);
        if (!tb || tb > 0xFFFFFFFFu || buf_put(&post, enc, tb)) goto done;
        wr32(e + 20, (uint32_t)db); wr32(e + 24, (uint32_t)tb);
        if (buf_put(&terms, e, TERM_SZ)) goto done;
        nterms++; i = j;
    }
    for (size_t i = 0; i < b->nd; i++) {
        BDoc *d = &b->docs[i]; uint8_t e[DOC_SZ] = {0};
        wr64(e, d->fp);
        if (heap_str(&heap, d->title, d->title_len, e + 8) || heap_str(&heap, d->cap, d->cap_len, e + 16)) goto done;
        wr32(e + 24, d->ntok);
        if (heap_str(&heap, d->url, d->url_len, e + 28) || heap_str(&heap, d->wfile, d->wfile_len, e + 36)) goto done;
        wr32(e + 44, d->wlen); wr64(e + 48, d->woff);
        if (heap_str(&heap, d->digest, d->digest_len, e + 56)) goto done;
        total_tokens += d->ntok;
        if (buf_put(&docs, e, DOC_SZ)) goto done;
    }
    {
        uint64_t terms_off = HDR, post_off = terms_off + terms.n;
        uint64_t docs_off = (post_off + post.n + 7) & ~7ULL, heap_off = docs_off + docs.n;
        uint8_t h[HDR] = {0}; static const uint8_t zero[8] = {0};
        memcpy(h, MAGIC, 8); wr32(h + 8, (uint32_t)b->nd); wr32(h + 12, nterms);
        wr64(h + 16, terms_off); wr64(h + 24, post_off); wr64(h + 32, post.n);
        wr64(h + 40, docs_off); wr64(h + 48, heap_off); wr64(h + 56, total_tokens);
        if (buf_put(&file, h, HDR) || buf_put(&file, terms.p, terms.n) || buf_put(&file, post.p, post.n) ||
            buf_put(&file, zero, (size_t)(docs_off - post_off - post.n)) ||
            buf_put(&file, docs.p, docs.n) || buf_put(&file, heap.p, heap.n)) goto done;
        DalwSha256 s; uint8_t dg[32];
        dalw_sha256_init(&s); dalw_sha256_update(&s, file.p, file.n); dalw_sha256_final(&s, dg);
        if (buf_put(&file, dg, 32)) goto done;
#ifdef _WIN32
        int wfd = create_shard(path);
        FILE *f = NULL;
        if (wfd >= 0) {
            f = _fdopen(wfd, "wb");
            if (!f) _close(wfd);
        }
#else
        FILE *f = fopen(path, "wbx");   /* "x" = fail if the file already exists */
#endif
        if (!f) goto done;
        size_t w = fwrite(file.p, 1, file.n, f);
        int cl = fclose(f);
        if (w != file.n || cl != 0) {
#ifdef _WIN32
            discard_shard(path);
#else
            remove(path);
#endif
            goto done;
        }
        rc = 0;
    }
done:
    free(tmp); free(tf); free(enc); free(terms.p); free(post.p); free(docs.p); free(heap.p); free(file.p);
    return rc;
}

/* ================= reader ================= */

void dalw_close(DalwShard *s) {
    if (!s) return;
    if (s->base) {
#ifdef DALW_MMAP
        if (s->mapped) munmap((void *)s->base, s->size); else
#endif
        free((void *)s->base);
    }
    memset(s, 0, sizeof *s);
}

int dalw_open(DalwShard *s, const char *path, int verify) {
    if (!s || !path) return -1;
    memset(s, 0, sizeof *s);
    uint8_t *base = NULL; size_t size = 0; int mapped = 0;
#ifdef DALW_MMAP
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)(HDR + 32)) { close(fd); return -1; }
    size = (size_t)st.st_size;
    void *m = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return -1;
    base = (uint8_t *)m; mapped = 1;
#else
#ifdef _WIN32
    FILE *f = read_shard(path);
#else
    FILE *f = fopen(path, "rb");
#endif
    if (!f) return -1;
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    int64_t L = _ftelli64(f);
    if (L < HDR + 32 || (uint64_t)L > SIZE_MAX || _fseeki64(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
#else
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long L = ftell(f);
    if (L < (long)(HDR + 32) || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
#endif
    size = (size_t)L; base = (uint8_t *)malloc(size);
    if (!base || fread(base, 1, size, f) != size) { free(base); fclose(f); return -1; }
    fclose(f);
#endif
    s->base = base; s->size = size; s->mapped = mapped;
    uint64_t body = size - 32;
    if (memcmp(base, MAGIC, 8) != 0) goto bad;
    s->ndocs = rd32(base + 8); s->nterms = rd32(base + 12);
    uint64_t to = rd64(base + 16), po = rd64(base + 24), ps = rd64(base + 32),
             dof = rd64(base + 40), ho = rd64(base + 48);
    s->total_tokens = rd64(base + 56);
    if (to != HDR || !in_range(to, (uint64_t)s->nterms * TERM_SZ, body)) goto bad;
    if (!in_range(po, ps, body) || po < to + (uint64_t)s->nterms * TERM_SZ) goto bad;
    if (!in_range(dof, (uint64_t)s->ndocs * DOC_SZ, body) || dof < po + ps) goto bad;
    if (ho != dof + (uint64_t)s->ndocs * DOC_SZ || ho > body) goto bad;
    s->terms = base + to; s->post = base + po; s->post_sz = (size_t)ps;
    s->docs = base + dof; s->heap = base + ho; s->heap_sz = (size_t)(body - ho);
    memcpy(s->sha256, base + body, 32);
    if (verify) {
        DalwSha256 h; uint8_t dg[32];
        dalw_sha256_init(&h); dalw_sha256_update(&h, base, (size_t)body); dalw_sha256_final(&h, dg);
        if (memcmp(dg, s->sha256, 32) != 0) { dalw_close(s); return -2; }
        s->signed_flag = 1;
    }
    return 0;
bad:
    dalw_close(s); return -3;
}

int dalw_doc(const DalwShard *s, uint32_t id, DalwDoc *d) {
    if (!s || !d || !s->base || id >= s->ndocs) return -1;
    const uint8_t *e = s->docs + (size_t)id * DOC_SZ;
    uint32_t to = rd32(e + 8), tl = rd32(e + 12), co = rd32(e + 16), cl = rd32(e + 20);
    uint32_t uo = rd32(e + 28), ul = rd32(e + 32), wo = rd32(e + 36), wl = rd32(e + 40);
    uint32_t go = rd32(e + 56), gl = rd32(e + 60);
    if (!in_range(to, tl, s->heap_sz) || !in_range(co, cl, s->heap_sz) || !in_range(uo, ul, s->heap_sz) ||
        !in_range(wo, wl, s->heap_sz) || !in_range(go, gl, s->heap_sz)) return -1;
    d->fp = rd64(e);
    d->title = (const char *)s->heap + to; d->title_len = tl;
    d->cap = (const char *)s->heap + co; d->cap_len = cl; d->ntok = rd32(e + 24);
    d->url = (const char *)s->heap + uo; d->url_len = ul;
    d->warc_file = (const char *)s->heap + wo; d->warc_file_len = wl;
    d->warc_len = rd32(e + 44); d->warc_offset = rd64(e + 48);
    d->digest = (const char *)s->heap + go; d->digest_len = gl;
    return 0;
}

/* ================= query / BM25 ================= */

typedef struct { const uint8_t *dp, *tp; uint32_t df, dbytes, tbytes; } QTerm;

static int find_term(const DalwShard *s, uint64_t h, QTerm *q) {
    uint32_t lo = 0, hi = s->nterms;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (rd64(s->terms + (size_t)mid * TERM_SZ) < h) lo = mid + 1; else hi = mid;
    }
    if (lo >= s->nterms) return 0;
    const uint8_t *e = s->terms + (size_t)lo * TERM_SZ;
    if (rd64(e) != h) return 0;
    uint64_t off = rd64(e + 8);
    q->df = rd32(e + 16); q->dbytes = rd32(e + 20); q->tbytes = rd32(e + 24);
    if (q->df == 0 || q->df > s->ndocs || !in_range(off, (uint64_t)q->dbytes + q->tbytes, s->post_sz)) return -1;
    q->dp = s->post + off; q->tp = q->dp + q->dbytes; return 1;
}

/* Postings are sorted. Checking the last ID bounds the entire decoded list. */
static int load_postings(const DalwShard *s, const QTerm *term, uint32_t *out) {
    if (dalw_plist_decode(term->dp, term->dbytes, term->df, out) < 0) return -1;
    return out[term->df - 1] < s->ndocs ? 0 : -1;
}

/* resolves terms and intersects; *res malloc'd (caller frees), *cnt may be 0. returns <0 on corruption. */
static int intersect_terms(const DalwShard *s, const uint64_t *hashes, size_t n, QTerm *q,
                           uint32_t **res, size_t *cnt) {
    uint32_t *A = NULL, *B = NULL, *C = NULL; int st = 0;
    *res = NULL; *cnt = 0;
    for (size_t i = 0; i < n; i++) {
        int r = find_term(s, hashes[i], &q[i]);
        if (r < 0) return -1;
        if (r == 0) return 0;
    }
    for (size_t i = 1; i < n; i++) {
        QTerm t = q[i]; size_t j = i;
        while (j > 0 && q[j-1].df > t.df) { q[j] = q[j-1]; j--; }
        q[j] = t;
    }
    A = (uint32_t *)malloc(((size_t)q[0].df + DALW_SLACK) * 4);
    C = (uint32_t *)malloc(((size_t)q[0].df + DALW_SLACK) * 4);
    B = (uint32_t *)malloc(((size_t)q[n-1].df + DALW_SLACK) * 4);
    if (!A || !B || !C) { st = -1; goto fail; }
    if (load_postings(s, &q[0], A) < 0) { st = -1; goto fail; }
    size_t cur = q[0].df;
    for (size_t i = 1; i < n && cur; i++) {
        if (load_postings(s, &q[i], B) < 0) { st = -1; goto fail; }
        cur = dalw_isect(A, cur, B, q[i].df, C);
        uint32_t *t = A; A = C; C = t;
    }
    free(B); free(C);
    *res = A; *cnt = cur; return 0;
fail:
    free(A); free(B); free(C); return st;
}

size_t dalw_query(const DalwShard *s, const uint64_t *hashes, size_t n,
                  uint32_t *out, size_t cap, int *status) {
    size_t res = 0, cnt = 0; uint32_t *A = NULL; QTerm *q = NULL; int st = 0;
    if (status) *status = 0;
    if (!s || !s->base || !hashes || !n || !out || !cap) return 0;
    q = n <= SIZE_MAX / sizeof *q ? (QTerm *)malloc(n * sizeof *q) : NULL;
    if (!q) { if (status) *status = -1; return 0; }
    st = intersect_terms(s, hashes, n, q, &A, &cnt);
    if (st >= 0 && cnt) { res = cnt < cap ? cnt : cap; memcpy(out, A, res * sizeof(uint32_t)); }
    free(q); free(A);
    if (status) *status = st;
    return st < 0 ? 0 : res;
}

size_t dalw_search(const DalwShard *s, const uint64_t *hashes, size_t n,
                   DalwHit *out, size_t k, int *status) {
    size_t cnt = 0, m = 0; uint32_t *cand = NULL, *D = NULL, *T = NULL; float *sc = NULL;
    QTerm *q = NULL; int st = 0;
    if (status) *status = 0;
    if (!s || !s->base || !hashes || !n || !out || !k) return 0;
    q = n <= SIZE_MAX / sizeof *q ? (QTerm *)malloc(n * sizeof *q) : NULL;
    if (!q) { st = -1; goto done; }
    st = intersect_terms(s, hashes, n, q, &cand, &cnt);
    if (st < 0 || cnt == 0) goto done;
    uint32_t maxdf = 0;
    for (size_t i = 0; i < n; i++) if (q[i].df > maxdf) maxdf = q[i].df;
    sc = (float *)calloc(cnt, sizeof(float));
    D = (uint32_t *)malloc(((size_t)maxdf + DALW_SLACK) * 4);
    T = (uint32_t *)malloc(((size_t)maxdf + DALW_SLACK) * 4);
    if (!sc || !D || !T) { st = -1; goto done; }
    double avgdl = s->ndocs ? (double)s->total_tokens / s->ndocs : 1.0;
    if (avgdl < 1.0) avgdl = 1.0;
    for (size_t t = 0; t < n; t++) {
        if (load_postings(s, &q[t], D) < 0 ||
            dalw_tf_decode(q[t].tp, q[t].tbytes, q[t].df, T) < 0) { st = -1; goto done; }
        float idf = (float)log(1.0 + ((double)s->ndocs - q[t].df + 0.5) / (q[t].df + 0.5));
        size_t p = 0;
        for (size_t ci = 0; ci < cnt; ci++) {
            while (p < q[t].df && D[p] < cand[ci]) p++;
            if (p < q[t].df && D[p] == cand[ci]) {
                float tfv = (float)T[p];
                float dl = (float)rd32(s->docs + (size_t)cand[ci] * DOC_SZ + 24);
                sc[ci] += idf * tfv * (BM25_K1 + 1.0f) /
                          (tfv + BM25_K1 * (1.0f - BM25_B + BM25_B * dl / (float)avgdl));
            }
        }
    }
    for (size_t ci = 0; ci < cnt; ci++) {
        float score = sc[ci];
        if (m == k && !(score > out[m-1].score)) continue;
        size_t pos = m < k ? m : k - 1;
        if (m < k) m++;
        while (pos > 0 && out[pos-1].score < score) { out[pos] = out[pos-1]; pos--; }
        out[pos].doc = cand[ci]; out[pos].score = score;
    }
done:
    free(q); free(cand); free(D); free(T); free(sc);
    if (status) *status = st;
    return st < 0 ? 0 : m;
}

/* ================= receipts ================= */

typedef struct { char *p; size_t cap, n; int err; } SB;
static void sb_raw(SB *b, const char *s, size_t n) {
    if (b->err) return;
    if (n > (size_t)INT_MAX - b->n || (b->p && n >= b->cap - b->n)) {
        b->err = 1;
        return;
    }
    if (b->p) {
        memcpy(b->p + b->n, s, n);
        b->p[b->n + n] = 0;
    }
    b->n += n;
}
static void sb_fmt(SB *b, const char *fmt, ...) {
    char tmp[256]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) { b->err = 1; return; }
    sb_raw(b, tmp, (size_t)n);
}
static void sb_json(SB *b, const char *s, size_t len) {
    const unsigned char *u = (const unsigned char *)s; size_t i = 0;
    sb_raw(b, "\"", 1);
    while (i < len) {
        unsigned c = u[i];
        if (c == '"') { sb_raw(b, "\\\"", 2); i++; }
        else if (c == '\\') { sb_raw(b, "\\\\", 2); i++; }
        else if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); sb_raw(b, t, 6); i++; }
        else if (c < 0x80) { sb_raw(b, s + i, 1); i++; }
        else {
            size_t j = i; uint32_t cp = utf8_next(u, len, &j);
            if (cp == 0xFFFD && j - i == 1) sb_raw(b, "?", 1); else sb_raw(b, s + i, j - i);
            i = j;
        }
    }
    sb_raw(b, "\"", 1);
}
static void hex_bytes(char *o, const uint8_t *p, size_t n) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { o[2*i] = H[p[i] >> 4]; o[2*i+1] = H[p[i] & 15]; }
    o[2*n] = 0;
}

int dalw_receipt(const DalwShard *s, const DalwHit *hit, const uint64_t *hashes, size_t n,
                 char *out, size_t cap) {
    DalwDoc d; SB b = {out, cap, 0, 0}; char hx[65];
    if (out && cap) out[0] = 0;
    if (!s || !hit || (!hashes && n) || (out == NULL) != (cap == 0) ||
        dalw_doc(s, hit->doc, &d) != 0) return -1;
    hex_bytes(hx, s->sha256, 32);
    sb_fmt(&b, "{\"v\":1,\"shard\":\"%s\",\"doc\":%u,\"fp\":\"%016llx\",\"url\":", hx, hit->doc, (unsigned long long)d.fp);
    sb_json(&b, d.url, d.url_len);
    sb_raw(&b, ",\"title\":", 9); sb_json(&b, d.title, d.title_len);
    sb_raw(&b, ",\"warc\":{\"file\":", 16); sb_json(&b, d.warc_file, d.warc_file_len);
    sb_fmt(&b, ",\"offset\":%llu,\"length\":%u},\"digest\":", (unsigned long long)d.warc_offset, d.warc_len);
    sb_json(&b, d.digest, d.digest_len);
    sb_fmt(&b, ",\"ntok\":%u,\"score\":%.6f,\"terms\":[", d.ntok, (double)hit->score);
    for (size_t i = 0; i < n; i++) sb_fmt(&b, "%s\"%016llx\"", i ? "," : "", (unsigned long long)hashes[i]);
    sb_raw(&b, "]", 1);
    char id[65] = {0};
    if (!b.err && out) {
        DalwSha256 h; uint8_t dg[32];
        dalw_sha256_init(&h); dalw_sha256_update(&h, out, b.n); dalw_sha256_final(&h, dg);
        hex_bytes(id, dg, 32);
    } else if (!out) {
        memset(id, '0', 64);
    }
    sb_fmt(&b, ",\"id\":\"%s\"}", id);
    if (b.err && out) out[0] = 0;
    return b.err ? -1 : (int)b.n;
}
