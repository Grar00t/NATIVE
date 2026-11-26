#ifndef NIYAH_DALW_H
#define NIYAH_DALW_H

/*
 * niyah_dalw: adapter between Niyah's search types and the dalw shard engine.
 * Read-only, local-only (no network, no telemetry). C11.
 *
 * document_id of a hit = the page fingerprint stored in the shard (DalwDoc.fp),
 * so the same page found in two shards has the same id.
 */

#include <stddef.h>
#include <stdint.h>

#include "niyah_search.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NIYAH_DALW_MAX_SHARDS 64

typedef enum {
    NIYAH_DALW_OK = 0,
    NIYAH_DALW_E_ARG = -1,       /* bad argument / empty query            */
    NIYAH_DALW_E_IO = -2,        /* cannot open or read the shard         */
    NIYAH_DALW_E_HASH = -3,      /* shard SHA-256 mismatch (tampered)     */
    NIYAH_DALW_E_FORMAT = -4,    /* not a valid shard                     */
    NIYAH_DALW_E_CORRUPT = -5,   /* corruption detected during a search   */
    NIYAH_DALW_E_MEM = -6,
    NIYAH_DALW_E_TOO_MANY = -7   /* more than NIYAH_DALW_MAX_SHARDS       */
} NiyahDalwStatus;

typedef struct NiyahDalw NiyahDalw;

typedef struct {
    uint64_t document_id;  /* page fingerprint (stable across shards) */
    float score;           /* BM25 score from dalw                    */
    uint32_t shard;        /* index into the paths given to open      */
    uint32_t doc;          /* document number inside that shard       */
} NiyahDalwHit;

/* Opens n shards. verify != 0 re-checks each shard's SHA-256 on open.
 * Returns NULL on failure and sets *status (status may be NULL). */
NiyahDalw *niyah_dalw_open(const char *const *paths, size_t n, int verify, int *status);
void niyah_dalw_close(NiyahDalw *handle);

size_t niyah_dalw_shard_count(const NiyahDalw *handle);
size_t niyah_dalw_doc_count(const NiyahDalw *handle);

/* AND-query over all shards; best first. exact != 0 disables Arabic stemming.
 * Returns the number of hits written (<= cap). */
size_t niyah_dalw_query(const NiyahDalw *handle, const char *query, int exact,
                        NiyahDalwHit *hits, size_t cap, int *status);

/* One-line JSON receipt for a hit (same query/exact as the query call).
 * Pass NULL, 0 to measure. Returns bytes excluding NUL, or -1 on error. */
int niyah_dalw_receipt(const NiyahDalw *handle, const NiyahDalwHit *hit,
                       const char *query, int exact, char *out, size_t cap);

/* Copy hits into Niyah's own hit type. Returns the number written. */
size_t niyah_dalw_to_search_hits(const NiyahDalwHit *in, size_t n,
                                 NiyahSearchHit *out, size_t cap);

const char *niyah_dalw_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif
