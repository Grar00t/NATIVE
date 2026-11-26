/* dalw.c v2: tokenizer + Arabic light stemmer, posting lists (+tf), shard builder/reader,
   AND query, BM25 top-k, receipts. C11, little-endian. */
#define _POSIX_C_SOURCE 200809L
#include "dalw_analysis.h"
#include "../native/niyah_utf8.h"
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
        uint32_t c = norm_cp(niyah_utf8_next(s, len, &i));
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

enum { POSTING_BLOCK = 128 };

static size_t block_count(uint32_t df) {
    return df / POSTING_BLOCK + (df % POSTING_BLOCK != 0);
}

static uint32_t block_length(uint32_t df, size_t block) {
    size_t remaining = df - block * POSTING_BLOCK;
    return remaining < POSTING_BLOCK ? (uint32_t)remaining : POSTING_BLOCK;
}

static int decode_docs_block(const uint8_t *p, size_t bytes, uint32_t df, size_t b, uint32_t *out) {
    size_t table = block_count(df) * 8;
    if (bytes < table) return -1;
    size_t pos = rd32(p + b * 8 + 4);
    if (pos < table || pos >= bytes) return -1;
    unsigned width = p[pos++];
    uint32_t count = block_length(df, b);
    if (width > 32 || ((size_t)(count - 1) * width + 7) / 8 > bytes - pos) return -1;
    uint64_t doc = rd32(p + b * 8);
    if (!width) {
        if (doc + count - 1 > UINT32_MAX) return -1;
        for (uint32_t i = 0; i < count; ++i) out[i] = (uint32_t)doc + i;
        return 0;
    }
    uint64_t bits = 0, mask = (UINT64_C(1) << width) - 1;
    unsigned available = 0;
    out[0] = (uint32_t)doc;
    for (uint32_t i = 1; i < count; ++i) {
        while (available < width) { bits |= (uint64_t)p[pos++] << available; available += 8; }
        doc += (bits & mask) + 1;
        if (doc > UINT32_MAX) return -1;
        out[i] = (uint32_t)doc;
        bits >>= width;
        available -= width;
    }
    return 0;
}

int dalw_plist_decode(const uint8_t *p, size_t bytes, uint32_t df, uint32_t *out) {
    for (size_t b = 0; b < block_count(df); ++b) {
        uint32_t *block = out + b * POSTING_BLOCK;
        if (decode_docs_block(p, bytes, df, b, block) < 0 || (b && block[0] <= block[-1])) return -1;
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

static int decode_tf_block(const uint8_t *p, size_t bytes, size_t *offset, uint32_t count, uint32_t *out) {
    size_t pos = *offset;
    if (pos >= bytes) return -1;
    unsigned width = p[pos++];
    if (width > 32 || ((size_t)count * width + 7) / 8 > bytes - pos) return -1;
    if (!width) {
        for (uint32_t i = 0; i < count; ++i) out[i] = 1;
        *offset = pos;
        return 0;
    }
    uint64_t bits = 0, mask = (UINT64_C(1) << width) - 1;
    unsigned available = 0;
    for (uint32_t i = 0; i < count; ++i) {
        while (available < width) { bits |= (uint64_t)p[pos++] << available; available += 8; }
        uint32_t value = (uint32_t)(bits & mask);
        if (value == UINT32_MAX) return -1;
        out[i] = value + 1;
        bits >>= width;
        available -= width;
    }
    *offset = pos;
    return 0;
}

int dalw_tf_decode(const uint8_t *p, size_t bytes, uint32_t df, uint32_t *out) {
    size_t pos = 0;
    for (size_t b = 0; b < block_count(df); ++b) {
        if (decode_tf_block(p, bytes, &pos, block_length(df, b), out + b * POSTING_BLOCK) < 0) return -1;
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

static int read_term(const DalwShard *s, const uint8_t *e, QTerm *q) {
    uint64_t off = rd64(e + 8);
    q->df = rd32(e + 16); q->dbytes = rd32(e + 20); q->tbytes = rd32(e + 24);
    if (q->df == 0 || q->df > s->ndocs || !in_range(off, (uint64_t)q->dbytes + q->tbytes, s->post_sz)) return -1;
    q->dp = s->post + off; q->tp = q->dp + q->dbytes; return 1;
}

static int find_term(const DalwShard *s, uint64_t h, QTerm *q) {
    uint32_t lo = 0, hi = s->nterms;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (rd64(s->terms + (size_t)mid * TERM_SZ) < h) lo = mid + 1; else hi = mid;
    }
    if (lo >= s->nterms) return 0;
    const uint8_t *e = s->terms + (size_t)lo * TERM_SZ;
    if (rd64(e) != h) return 0;
    return read_term(s, e, q);
}

int dalw_document_frequency(const DalwShard *s, uint64_t term, uint32_t *df) {
    QTerm q;
    int found = find_term(s, term, &q);
    *df = found > 0 ? q.df : 0;
    return found;
}


/* Postings are sorted. Checking the last ID bounds the entire decoded list. */
static int load_postings(const DalwShard *s, const QTerm *term, uint32_t *out) {
    if (!term->df || dalw_plist_decode(term->dp, term->dbytes, term->df, out) < 0) return -1;
    return out[term->df - 1] < s->ndocs ? 0 : -1;
}

/* Stable document-frequency order also fixes the floating-point addition order. */
static int resolve_terms(const DalwShard *s, const uint64_t *keys, size_t n, int any,
                         QTerm *terms, size_t *count) {
    *count = 0;
    for (size_t i = 0; i < n; ++i) {
        QTerm term;
        int found = find_term(s, keys[i], &term);
        if (found < 0) return -1;
        if (!found) {
            if (!any) { *count = 0; break; }
            continue;
        }
        size_t j = (*count)++;
        while (j && terms[j - 1].df > term.df) { terms[j] = terms[j - 1]; --j; }
        terms[j] = term;
    }
    return 0;
}

/* resolves terms and intersects; *res malloc'd (caller frees), *cnt may be 0. returns <0 on corruption. */
static int intersect_terms(const DalwShard *s, const uint64_t *hashes, size_t n, QTerm *q,
                           uint32_t **res, size_t *cnt) {
    uint32_t *A = NULL, *B = NULL, *C = NULL; int st = 0;
    *res = NULL; *cnt = 0;
    size_t count;
    if (resolve_terms(s, hashes, n, 0, q, &count) < 0) return -1;
    if (!count) return 0;
    A = (uint32_t *)malloc(((size_t)q[0].df + DALW_SLACK) * 4);
    if (!A || load_postings(s, &q[0], A) < 0) { st = -1; goto fail; }
    if (n == 1) { *res = A; *cnt = q[0].df; return 0; }
    C = (uint32_t *)malloc(((size_t)q[0].df + DALW_SLACK) * 4);
    B = (uint32_t *)malloc(((size_t)q[n-1].df + DALW_SLACK) * 4);
    if (!B || !C) { st = -1; goto fail; }
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

static int rank_hit(const void *left, const void *right) {
    const DalwHit *a = left, *b = right;
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    return (a->doc > b->doc) - (a->doc < b->doc);
}

static void sift_hit(DalwHit *heap, size_t n, size_t parent) {
    DalwHit saved = heap[parent];
    while (parent < n / 2) {
        size_t child = parent * 2 + 1;
        if (child + 1 < n && rank_hit(&heap[child + 1], &heap[child]) > 0) ++child;
        if (rank_hit(&saved, &heap[child]) >= 0) break;
        heap[parent] = heap[child];
        parent = child;
    }
    heap[parent] = saved;
}

static float bm25_hit(const DalwShard *s, uint32_t doc, uint32_t frequency, float idf, float avg) {
    float tf = (float)frequency;
    float length = (float)rd32(s->docs + (size_t)doc * DOC_SZ + 24);
    return idf * tf * (BM25_K1 + 1.0f) /
           (tf + BM25_K1 * (1.0f - BM25_B + BM25_B * length / avg));
}

/* Candidates arrive in doc order, so an equal score cannot displace an earlier hit. */
static void keep_small(DalwHit *out, size_t used, size_t cap, DalwHit hit) {
    if (used == cap && hit.score <= out[cap - 1].score) return;
    size_t pos = used < cap ? used : cap - 1;
    while (pos && out[pos - 1].score < hit.score) {
        out[pos] = out[pos - 1];
        --pos;
    }
    out[pos] = hit;
}

typedef struct {
    const QTerm *term;
    float idf;
    size_t block, tf_offset;
    uint32_t docs[POSTING_BLOCK], tf[POSTING_BLOCK], at, count;
} PostingCursor;

/* An exhausted cursor has count=0. The next block must begin after the previous one. */
static int next_block(PostingCursor *c, uint32_t ndocs) {
    const QTerm *t = c->term;
    if (c->block == block_count(t->df)) { c->count = 0; return 0; }
    uint32_t previous = c->count ? c->docs[c->count - 1] : 0;
    c->count = block_length(t->df, c->block);
    c->at = 0;
    if (decode_docs_block(t->dp, t->dbytes, t->df, c->block, c->docs) < 0 ||
        (c->block && c->docs[0] <= previous) || c->docs[c->count - 1] >= ndocs ||
        decode_tf_block(t->tp, t->tbytes, &c->tf_offset, c->count, c->tf) < 0) return -1;
    ++c->block;
    return 0;
}

int dalw_walk_postings(const DalwShard *s, DalwPostingBlock visit, void *context) {
    uint64_t previous = 0;
    for (uint32_t i = 0; i < s->nterms; ++i) {
        const uint8_t *entry = s->terms + (size_t)i * TERM_SZ;
        uint64_t hash = rd64(entry);
        QTerm term;
        if ((i && hash <= previous) || read_term(s, entry, &term) < 0) return -1;
        previous = hash;
        PostingCursor cursor = {.term = &term};
        for (;;) {
            if (next_block(&cursor, s->ndocs) < 0) return -1;
            if (!cursor.count) break;
            if (visit(hash, term.df, cursor.docs, cursor.tf, cursor.count, context) != 0)
                return -1;
        }
        if (cursor.tf_offset != term.tbytes) return -1;
    }
    return 0;
}

enum { SMALL_TOP_K = 64 };

typedef struct { DalwHit *hits; size_t seen, cap; } TopHits;

static inline void keep_ranked(DalwHit *out, size_t seen, size_t limit, int needs_heap, DalwHit hit) {
    if (limit <= SMALL_TOP_K) {
        keep_small(out, seen < limit ? seen : limit, limit, hit);
    } else if (seen < limit) {
        out[seen] = hit;
        if (seen + 1 == limit && needs_heap)
            for (size_t i = limit / 2; i; --i) sift_hit(out, limit, i - 1);
    } else if (hit.score > out[0].score) {
        out[0] = hit;
        sift_hit(out, limit, 0);
    }
}

/* A single term has exactly df hits. Use each block's position in that sequence. */
static int score_single(const DalwShard *s, PostingCursor *term, float avg, TopHits *top) {
    const size_t df = term->term->df;
    const size_t limit = top->cap < df ? top->cap : df;
    size_t seen = 0;
    while (term->count) {
        for (uint32_t row = 0; row < term->count; ++row) {
            uint32_t doc = term->docs[row];
            DalwHit hit = {doc, bm25_hit(s, doc, term->tf[row], term->idf, avg)};
            keep_ranked(top->hits, seen + row, limit, df > limit, hit);
        }
        seen += term->count;
        if (next_block(term, s->ndocs) < 0) return -1;
    }
    top->cap = limit;
    top->seen = seen;
    return 0;
}

static int score_all(const DalwShard *s, PostingCursor *c, size_t n, float avg, TopHits *top) {
    PostingCursor *lead = c;
    while (lead->count) {
        for (uint32_t row = 0; row < lead->count; ++row) {
            uint32_t doc = lead->docs[row];
            float score = bm25_hit(s, doc, lead->tf[row], lead->idf, avg);
            size_t i = 1;
            for (; i < n; ++i) {
                PostingCursor *term = &c[i];
                while (term->count && term->docs[term->count - 1] < doc)
                    if (next_block(term, s->ndocs) < 0) return -1;
                if (!term->count) break;
                while (term->docs[term->at] < doc) ++term->at;
                if (term->docs[term->at] != doc) break;
                score += bm25_hit(s, doc, term->tf[term->at], term->idf, avg);
            }
            if (i == n) keep_ranked(top->hits, top->seen++, top->cap, 1, (DalwHit){doc, score});
        }
        if (next_block(lead, s->ndocs) < 0) return -1;
    }
    /* Validate the tail even when the shortest list ends in an earlier block. */
    for (size_t i = 1; i < n; ++i)
        while (c[i].count) if (next_block(&c[i], s->ndocs) < 0) return -1;
    return 0;
}

static int score_any(const DalwShard *s, PostingCursor *c, size_t n, float avg, TopHits *top) {
    enum { WINDOW = 256 };
    for (;;) {
        uint32_t first = UINT32_MAX;
        for (size_t i = 0; i < n; ++i)
            if (c[i].count && c[i].docs[c[i].at] < first) first = c[i].docs[c[i].at];
        if (first == UINT32_MAX) return 0;
        uint32_t base = first - first % WINDOW;
        float scores[WINDOW] = {0};
        for (size_t i = 0; i < n; ++i) {
            PostingCursor *term = &c[i];
            while (term->count) {
                uint32_t doc = term->docs[term->at];
                if (doc - base >= WINDOW) break;
                scores[doc - base] += bm25_hit(s, doc, term->tf[term->at], term->idf, avg);
                if (++term->at == term->count && next_block(term, s->ndocs) < 0) return -1;
            }
        }
        for (unsigned i = 0; i < WINDOW; ++i)
            if (scores[i] > 0.0f)
                keep_ranked(top->hits, top->seen++, top->cap, 1, (DalwHit){base + i, scores[i]});
    }
}

size_t dalw_search_mode(const DalwShard *s, const uint64_t *hashes, size_t n, int any,
                        DalwHit *out, size_t k, int *status) {
    int st = 0;
    size_t found = 0;
    TopHits top = {.hits = out, .cap = k};
    QTerm one_term, *terms = &one_term;
    PostingCursor one_cursor, *c = &one_cursor;
    if (!s || !s->base || !hashes || !n || !out || !k) goto done;
    if (n > 1) terms = n <= SIZE_MAX / sizeof(*terms) ? malloc(n * sizeof(*terms)) : NULL;
    if (!terms) { st = -1; goto done; }
    st = resolve_terms(s, hashes, n, any, terms, &found);
    if (st < 0 || !found) goto done;
    if (found > 1) c = found <= SIZE_MAX / sizeof(*c) ? malloc(found * sizeof(*c)) : NULL;
    if (!c) { st = -1; goto done; }
    for (size_t i = 0; i < found; ++i) {
        c[i].block = c[i].tf_offset = 0;
        c[i].count = 0;
        c[i].term = &terms[i];
        c[i].idf = (float)log(1.0 + ((double)s->ndocs - terms[i].df + 0.5) / (terms[i].df + 0.5));
        if (next_block(&c[i], s->ndocs) < 0) { st = -1; goto done; }
    }
    double mean = (double)s->total_tokens / s->ndocs;
    float avg = mean < 1.0 ? 1.0f : (float)mean;
    if (found == 1) st = score_single(s, c, avg, &top);
    else if (any) st = score_any(s, c, found, avg, &top);
    else st = score_all(s, c, found, avg, &top);
    if (top.seen > top.cap) top.seen = top.cap;
    if (st == 0 && top.cap > SMALL_TOP_K) qsort(out, top.seen, sizeof(*out), rank_hit);
done:
    if (c != &one_cursor) free(c);
    if (terms != &one_term) free(terms);
    if (status) *status = st;
    return st < 0 ? 0 : top.seen;
}

size_t dalw_search(const DalwShard *s, const uint64_t *hashes, size_t n,
                   DalwHit *out, size_t k, int *status) {
    return dalw_search_mode(s, hashes, n, 0, out, k, status);
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
            size_t j = i; uint32_t cp = niyah_utf8_next(u, len, &j);
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

/* The candidate IDs are sorted. Decode a posting block only when a candidate enters it. */
int dalw_refine_docs(const DalwShard *s, uint64_t hash, uint32_t *docs, size_t *count) {
    QTerm t;
    int found = find_term(s, hash, &t);
    if (found < 0) return -1;
    if (!found) { *count = 0; return 0; }
    size_t blocks = block_count(t.df), block = blocks, used = 0;
    uint32_t ids[POSTING_BLOCK], length = 0, at = 0;
    if (t.dbytes < blocks * 8) return -1;
    for (size_t i = 0; i < *count; ++i) {
        uint32_t doc = docs[i];
        if (block == blocks || (block + 1 < blocks && rd32(t.dp + (block + 1) * 8) <= doc)) {
            size_t lo = block == blocks ? 0 : block + 1;
            size_t hi = blocks;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (rd32(t.dp + mid * 8) <= doc) lo = mid + 1; else hi = mid;
            }
            if (!lo) continue;
            block = lo - 1;
            length = block_length(t.df, block);
            if (decode_docs_block(t.dp, t.dbytes, t.df, block, ids) < 0 ||
                ids[length - 1] >= s->ndocs) return -1;
            at = 0;
        }
        while (at < length && ids[at] < doc) ++at;
        if (at < length && ids[at] == doc) docs[used++] = doc;
    }
    *count = used;
    return 0;
}

static int term_contains(const DalwShard *s, uint64_t hash, uint32_t doc) {
    size_t count = 1;
    return dalw_refine_docs(s, hash, &doc, &count) < 0 ? -1 : (int)count;
}

int dalw_receipt_mode(const DalwShard *s, const DalwHit *hit, const uint64_t *hashes, size_t n,
                      int any, char *out, size_t cap) {
    DalwDoc d; SB b = {out, cap, 0, 0}; char hx[65];
    if (out && cap) out[0] = 0;
    if (!s || !hit || (!hashes && n) || (out == NULL) != (cap == 0) ||
        dalw_doc(s, hit->doc, &d) != 0) return -1;
    hex_bytes(hx, s->sha256, 32);
    sb_fmt(&b, "{\"v\":%d,\"shard\":\"%s\",\"doc\":%u,\"fp\":\"%016llx\",\"url\":", any ? 2 : 1, hx, hit->doc, (unsigned long long)d.fp);
    sb_json(&b, d.url, d.url_len);
    sb_raw(&b, ",\"title\":", 9); sb_json(&b, d.title, d.title_len);
    sb_raw(&b, ",\"warc\":{\"file\":", 16); sb_json(&b, d.warc_file, d.warc_file_len);
    sb_fmt(&b, ",\"offset\":%llu,\"length\":%u},\"digest\":", (unsigned long long)d.warc_offset, d.warc_len);
    sb_json(&b, d.digest, d.digest_len);
    sb_fmt(&b, ",\"ntok\":%u,\"score\":%.6f,\"terms\":[", d.ntok, (double)hit->score);
    for (size_t i = 0; i < n; i++) sb_fmt(&b, "%s\"%016llx\"", i ? "," : "", (unsigned long long)hashes[i]);
    sb_raw(&b, "]", 1);
    if (any) {
        const char fields[] = ",\"match\":\"any\",\"matched_terms\":[";
        sb_raw(&b, fields, sizeof fields - 1);
        size_t matched = 0;
        for (size_t i = 0; i < n && !b.err; ++i) {
            int contains = term_contains(s, hashes[i], hit->doc);
            if (contains < 0) { b.err = 1; break; }
            if (contains) sb_fmt(&b, "%s\"%016llx\"", matched++ ? "," : "", (unsigned long long)hashes[i]);
        }
        if (!matched) b.err = 1;
        sb_raw(&b, "]", 1);
    }
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

int dalw_receipt(const DalwShard *s, const DalwHit *hit, const uint64_t *hashes, size_t n,
                 char *out, size_t cap) {
    return dalw_receipt_mode(s, hit, hashes, n, 0, out, cap);
}
