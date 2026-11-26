/* dalw v2: signed-shard Arabic-aware search index with BM25 + receipts. C11, little-endian. */
#ifndef DALW_H
#define DALW_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- SHA-256 ---- */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } DalwSha256;
void dalw_sha256_init(DalwSha256 *s);
void dalw_sha256_update(DalwSha256 *s, const void *p, size_t n);
void dalw_sha256_final(DalwSha256 *s, uint8_t out[32]);

/* ---- sorted-set intersection (inputs strictly increasing) ----
   scalar/avx2/gallop: `out` needs min(na,nb) + DALW_SLACK slots. avx512 uses compress-store (no overrun).
   Env DALW_ISECT=scalar|avx2|avx512 overrides the dispatcher. */
#define DALW_SLACK 8
size_t dalw_isect_scalar(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out);
size_t dalw_isect_gallop(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out);
size_t dalw_isect_avx2(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out);
size_t dalw_isect_avx512(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out);
size_t dalw_isect(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out);
int dalw_has_avx2(void);
int dalw_has_avx512(void);

/* ---- tokenizer + Arabic light stemmer ---- */
typedef void (*DalwTokFn)(uint64_t hash, void *user);
void dalw_tokens(const char *text, size_t len, DalwTokFn fn, void *user);   /* surface-form hashes */
uint64_t dalw_term_hash(const char *word);                                   /* first token, surface form */
#define DALW_Q_STEM  0   /* Arabic words match by stem, others by surface */
#define DALW_Q_EXACT 1   /* surface form only */
size_t dalw_parse_query(const char *text, int mode, uint64_t *out, size_t max);
size_t dalw_stem_utf8(const char *word, char *out, size_t cap);              /* first token's stem, for tests/debug */

/* ---- posting lists: 128-doc blocks, delta-1 + bit-packing, skip table; tf = bit-packed (tf-1) ---- */
size_t dalw_plist_bound(uint32_t n);
size_t dalw_plist_encode(const uint32_t *docs, uint32_t n, uint8_t *out);
int dalw_plist_decode(const uint8_t *p, size_t bytes, uint32_t df, uint32_t *out);
size_t dalw_tf_encode(const uint32_t *tf, uint32_t n, uint8_t *out);          /* out needs dalw_plist_bound(n) */
int dalw_tf_decode(const uint8_t *p, size_t bytes, uint32_t n, uint32_t *out);

/* ---- shard ---- */
typedef struct {
    const uint8_t *base; size_t size; int mapped;
    uint32_t ndocs, nterms;
    const uint8_t *terms, *post, *docs, *heap;
    size_t post_sz, heap_sz;
    uint64_t total_tokens;
    uint8_t sha256[32]; int signed_flag;   /* signed_flag = trailer SHA-256 verified (integrity, not authenticity) */
} DalwShard;

typedef struct {
    uint64_t fp; const char *title; uint32_t title_len;
    const char *cap; uint32_t cap_len; uint32_t ntok;
    const char *url; uint32_t url_len;
    const char *warc_file; uint32_t warc_file_len; uint64_t warc_offset; uint32_t warc_len;
    const char *digest; uint32_t digest_len;
} DalwDoc;

typedef struct {
    const char *title, *url, *warc_file, *digest;
    uint64_t warc_offset; uint32_t warc_len;
    const char *text; size_t text_len;
} DalwSrc;

typedef struct DalwBuilder DalwBuilder;
DalwBuilder *dalw_builder_new(void);
int dalw_builder_add(DalwBuilder *b, const char *title, const char *text, size_t len);
int dalw_builder_add_ex(DalwBuilder *b, const DalwSrc *src);
int dalw_builder_write(DalwBuilder *b, const char *path);   /* refuses to overwrite */
void dalw_builder_free(DalwBuilder *b);

int dalw_open(DalwShard *s, const char *path, int verify);  /* 0 ok, -1 io, -2 hash mismatch, -3 malformed */
void dalw_close(DalwShard *s);
int dalw_doc(const DalwShard *s, uint32_t id, DalwDoc *d);

/* AND query (doc ids ascending). *status < 0 on corruption. */
size_t dalw_query(const DalwShard *s, const uint64_t *hashes, size_t n,
                  uint32_t *out, size_t cap, int *status);

/* BM25 (k1=1.2, b=0.75) over the AND set, top-k, best first. */
typedef struct { uint32_t doc; float score; } DalwHit;
size_t dalw_search(const DalwShard *s, const uint64_t *hashes, size_t n,
                   DalwHit *out, size_t k, int *status);

/* One-line JSON receipt. id = SHA-256 of everything before ,"id":. With out=NULL and cap=0, measures the required bytes (excluding NUL). Returns length, or -1. */
int dalw_receipt(const DalwShard *s, const DalwHit *hit, const uint64_t *hashes, size_t n,
                 char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
