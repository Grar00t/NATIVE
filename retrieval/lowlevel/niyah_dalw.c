#define _POSIX_C_SOURCE 200809L

#include "niyah_dalw.h"

#include <stdlib.h>
#include <string.h>

#include "dalw.h"

struct NiyahDalw {
    DalwShard shards[NIYAH_DALW_MAX_SHARDS];
    size_t count;
};

static int map_open_error(int r)
{
    switch (r) {
    case -1: return NIYAH_DALW_E_IO;
    case -2: return NIYAH_DALW_E_HASH;
    case -3: return NIYAH_DALW_E_FORMAT;
    default: return NIYAH_DALW_E_IO;
    }
}

static void set_status(int *status, int value)
{
    if (status) *status = value;
}

NiyahDalw *niyah_dalw_open(const char *const *paths, size_t n, int verify, int *status)
{
    NiyahDalw *handle;
    size_t i;

    set_status(status, NIYAH_DALW_OK);
    if (!paths || n == 0u) {
        set_status(status, NIYAH_DALW_E_ARG);
        return NULL;
    }
    if (n > NIYAH_DALW_MAX_SHARDS) {
        set_status(status, NIYAH_DALW_E_TOO_MANY);
        return NULL;
    }
    handle = (NiyahDalw *)calloc(1u, sizeof *handle);
    if (!handle) {
        set_status(status, NIYAH_DALW_E_MEM);
        return NULL;
    }
    for (i = 0u; i < n; i++) {
        int r;
        if (!paths[i]) {
            set_status(status, NIYAH_DALW_E_ARG);
            niyah_dalw_close(handle);
            return NULL;
        }
        r = dalw_open(&handle->shards[i], paths[i], verify ? 1 : 0);
        if (r != 0) {
            set_status(status, map_open_error(r));
            niyah_dalw_close(handle);
            return NULL;
        }
        handle->count = i + 1u;
    }
    return handle;
}

void niyah_dalw_close(NiyahDalw *handle)
{
    size_t i;
    if (!handle) return;
    for (i = 0u; i < handle->count; i++) dalw_close(&handle->shards[i]);
    free(handle);
}

size_t niyah_dalw_shard_count(const NiyahDalw *handle)
{
    return handle ? handle->count : 0u;
}

size_t niyah_dalw_doc_count(const NiyahDalw *handle)
{
    size_t i, total = 0u;
    if (!handle) return 0u;
    for (i = 0u; i < handle->count; i++) total += (size_t)handle->shards[i].ndocs;
    return total;
}

static uint64_t document_id_for(const DalwShard *shard, uint32_t shard_index, uint32_t doc)
{
    DalwDoc d;
    memset(&d, 0, sizeof d);
    if (dalw_doc(shard, doc, &d) >= 0 && d.fp != 0u) return d.fp;
    /* fallback: unique per (shard, doc) when no fingerprint is available */
    return ((uint64_t)shard_index << 32) | (uint64_t)doc;
}

size_t niyah_dalw_query_mode(const NiyahDalw *handle, const char *query, int exact, int any,
                        NiyahDalwHit *hits, size_t cap, int *status)
{
    uint64_t terms[DALW_MAX_QUERY_TERMS];
    size_t nterms, i, total = 0u, slots = 0u, largest = 0u;
    NiyahDalwHit *best = NULL;
    DalwHit *tmp = NULL;

    set_status(status, NIYAH_DALW_OK);
    if (!handle || !query || !hits || cap == 0u) {
        set_status(status, NIYAH_DALW_E_ARG);
        return 0u;
    }
    nterms = dalw_parse_query(query, exact ? DALW_Q_EXACT : DALW_Q_STEM,
                              terms, DALW_MAX_QUERY_TERMS);
    if (nterms == 0u) {
        set_status(status, NIYAH_DALW_E_ARG);
        return 0u;
    }

    for (i = 0u; i < handle->count; ++i) {
        size_t count = handle->shards[i].ndocs;
        if (count > cap) count = cap;
        slots += count < cap - slots ? count : cap - slots;
        if (count > largest) largest = count;
    }
    if (slots == 0u) return 0u;
    if (slots > SIZE_MAX / sizeof *best || largest > SIZE_MAX / sizeof *tmp) {
        set_status(status, NIYAH_DALW_E_MEM);
        return 0u;
    }
    best = (NiyahDalwHit *)malloc(slots * sizeof *best);
    tmp = (DalwHit *)malloc(largest * sizeof *tmp);
    if (!best || !tmp) {
        free(best);
        free(tmp);
        set_status(status, NIYAH_DALW_E_MEM);
        return 0u;
    }

    for (i = 0u; i < handle->count; i++) {
        int st = 0;
        size_t m, per_shard = handle->shards[i].ndocs;
        if (per_shard > cap) per_shard = cap;
        m = dalw_search_mode(&handle->shards[i], terms, nterms, any, tmp, per_shard, &st);
        if (st < 0) {
            free(best);
            free(tmp);
            set_status(status, NIYAH_DALW_E_CORRUPT);
            return 0u;
        }
        if (!m) continue;
        /* Earlier shards win score ties. Select both prefixes, then merge backwards. */
        size_t old = 0, fresh = 0;
        while (old + fresh < slots && old < total && fresh < m) {
            if (best[old].score >= tmp[fresh].score) ++old;
            else ++fresh;
        }
        size_t room = slots - old - fresh;
        if (old == total) fresh += m - fresh < room ? m - fresh : room;
        else if (fresh == m) old += total - old < room ? total - old : room;
        total = old + fresh;
        while (fresh) {
            size_t dest = old + fresh - 1;
            if (old && best[old - 1].score < tmp[fresh - 1].score) {
                best[dest] = best[--old];
            } else {
                const DalwHit hit = tmp[--fresh];
                best[dest] = (NiyahDalwHit){.shard = (uint32_t)i, .doc = hit.doc, .score = hit.score};
            }
        }
    }

    for (i = 0; i < total; ++i) {
        hits[i] = best[i];
        hits[i].document_id = document_id_for(&handle->shards[best[i].shard], best[i].shard, best[i].doc);
    }
    free(best);
    free(tmp);
    return total;
}

size_t niyah_dalw_query(const NiyahDalw *handle, const char *query, int exact,
                        NiyahDalwHit *hits, size_t cap, int *status)
{
    return niyah_dalw_query_mode(handle, query, exact, 0, hits, cap, status);
}

int niyah_dalw_receipt_mode(const NiyahDalw *handle, const NiyahDalwHit *hit,
                            const char *query, int exact, int any, char *out, size_t cap)
{
    uint64_t terms[DALW_MAX_QUERY_TERMS];
    size_t nterms;
    DalwHit dh;

    if (out && cap) out[0] = '\0';
    if (!handle || !hit || !query || (out == NULL) != (cap == 0u)) return -1;
    if ((size_t)hit->shard >= handle->count) return -1;
    nterms = dalw_parse_query(query, exact ? DALW_Q_EXACT : DALW_Q_STEM,
                              terms, DALW_MAX_QUERY_TERMS);
    if (nterms == 0u) return -1;
    memset(&dh, 0, sizeof dh);
    dh.doc = hit->doc;
    dh.score = hit->score;
    return dalw_receipt_mode(&handle->shards[hit->shard], &dh, terms, nterms, any, out, cap);
}

int niyah_dalw_receipt(const NiyahDalw *handle, const NiyahDalwHit *hit,
                       const char *query, int exact, char *out, size_t cap)
{
    return niyah_dalw_receipt_mode(handle, hit, query, exact, 0, out, cap);
}

size_t niyah_dalw_to_search_hits(const NiyahDalwHit *in, size_t n,
                                 NiyahSearchHit *out, size_t cap)
{
    size_t i;
    if (!in || !out) return 0u;
    for (i = 0u; i < n && i < cap; i++) {
        out[i].document_id = in[i].document_id;
        out[i].score = (double)in[i].score;
    }
    return i;
}

const char *niyah_dalw_strerror(int status)
{
    switch (status) {
    case NIYAH_DALW_OK: return "ok";
    case NIYAH_DALW_E_ARG: return "bad argument or empty query";
    case NIYAH_DALW_E_IO: return "cannot open or read shard";
    case NIYAH_DALW_E_HASH: return "shard SHA-256 mismatch";
    case NIYAH_DALW_E_FORMAT: return "not a valid dalw shard";
    case NIYAH_DALW_E_CORRUPT: return "shard corruption detected";
    case NIYAH_DALW_E_MEM: return "out of memory";
    case NIYAH_DALW_E_TOO_MANY: return "too many shards";
    default: return "unknown error";
    }
}
