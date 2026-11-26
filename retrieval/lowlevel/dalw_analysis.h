#ifndef DALW_ANALYSIS_H
#define DALW_ANALYSIS_H

#include "dalw.h"

/* Internal read-only operations on version 2 postings. */
typedef int (*DalwPostingBlock)(uint64_t term, uint32_t df, const uint32_t *docs,
                                const uint32_t *tf, uint32_t count, void *context);
int dalw_walk_postings(const DalwShard *s, DalwPostingBlock visit, void *context);
int dalw_document_frequency(const DalwShard *s, uint64_t term, uint32_t *df);
int dalw_refine_docs(const DalwShard *s, uint64_t term, uint32_t *docs, size_t *count);

typedef struct DalwRows DalwRows;
DalwRows *dalw_rows_build(const DalwShard *s);
void dalw_rows_free(DalwRows *rows);
size_t dalw_rows_bytes(const DalwRows *rows);
/* ids has room for every document. The shard must outlive its row index. */
int dalw_rows_match(const DalwRows *rows, const uint64_t *terms, size_t n,
                    uint32_t *ids, size_t *matches, size_t *candidates);

typedef struct { uint64_t hash; uint32_t id; } DalwFingerprint;
/* Sorted by document number. Empty documents have no fingerprint. Caller frees *out. */
int dalw_fingerprints(const DalwShard *s, DalwFingerprint **out, size_t *count);
typedef int (*DalwPairFn)(uint32_t first, uint32_t second, unsigned distance, void *context);
int dalw_simhash_pairs(const DalwShard *s, unsigned radius, DalwPairFn report, void *context);

#endif
