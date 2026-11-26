#include "dalw_analysis.h"
#include <stdlib.h>

enum { RANKS = 3, ROWS = 1024 };

struct DalwRows {
    const DalwShard *shard;
    size_t words[RANKS], bytes;
    uint64_t *bank[RANKS];
};

/* Each rank folds pairs of 64-document words, preserving the bit lane.
   Rare terms get additional rows in independent banks. */
static unsigned term_ranks(uint32_t docs, uint32_t df)
{
    if (df <= docs / 64) return 3;
    if (df <= docs / 8) return 2;
    return 1;
}

static uint64_t *term_row(const DalwRows *rows, uint64_t hash, unsigned rank)
{
    size_t slot = (hash >> (21 * rank)) & (ROWS - 1);
    return rows->bank[rank] + slot * rows->words[rank];
}

static int insert_block(uint64_t hash, uint32_t df, const uint32_t *docs,
                        const uint32_t *tf, uint32_t count, void *context)
{
    DalwRows *rows = context;
    unsigned ranks = term_ranks(rows->shard->ndocs, df);
    (void)tf;
    for (unsigned rank = 0; rank < ranks; ++rank) {
        uint64_t *row = term_row(rows, hash, rank);
        for (uint32_t i = 0; i < count; ++i)
            row[docs[i] >> (6 + rank)] |= UINT64_C(1) << (docs[i] & 63);
    }
    return 0;
}

void dalw_rows_free(DalwRows *rows)
{
    if (!rows) return;
    for (unsigned rank = 0; rank < RANKS; ++rank) free(rows->bank[rank]);
    free(rows);
}

DalwRows *dalw_rows_build(const DalwShard *s)
{
    DalwRows *rows = calloc(1, sizeof *rows);
    if (!rows) return NULL;
    rows->shard = s;
    size_t words = s->ndocs / 64 + (s->ndocs % 64 != 0);
    for (unsigned rank = 0; rank < RANKS && words; ++rank) {
        if (words > (SIZE_MAX - rows->bytes) / (ROWS * sizeof(uint64_t))) goto fail;
        rows->words[rank] = words;
        rows->bank[rank] = calloc(words, ROWS * sizeof(uint64_t));
        if (!rows->bank[rank]) goto fail;
        rows->bytes += words * ROWS * sizeof(uint64_t);
        words = words / 2 + words % 2;
    }
    if (dalw_walk_postings(s, insert_block, rows) < 0) goto fail;
    return rows;
fail:
    dalw_rows_free(rows);
    return NULL;
}

size_t dalw_rows_bytes(const DalwRows *rows) { return rows->bytes; }

int dalw_rows_match(const DalwRows *rows, const uint64_t *terms, size_t n,
                    uint32_t *ids, size_t *matches, size_t *candidates)
{
    struct { const uint64_t *bits; unsigned rank; } selected[DALW_MAX_QUERY_TERMS * RANKS];
    size_t used = 0, count = 0;
    *candidates = *matches = 0;
    if (!n) return 0;
    if (n > DALW_MAX_QUERY_TERMS) return -1;
    for (size_t i = 0; i < n; ++i) {
        uint32_t df;
        int found = dalw_document_frequency(rows->shard, terms[i], &df);
        if (found <= 0) return found;
        unsigned ranks = term_ranks(rows->shard->ndocs, df);
        for (unsigned r = 0; r < ranks; ++r) {
            selected[used].bits = term_row(rows, terms[i], r);
            selected[used++].rank = r;
        }
    }
    for (size_t word = 0; word < rows->words[0]; ++word) {
        uint64_t bits = selected[0].bits[word];
        for (size_t r = 1; bits && r < used; ++r)
            bits &= selected[r].bits[word >> selected[r].rank];
        while (bits) {
            unsigned bit = (unsigned)__builtin_ctzll(bits);
            ids[count++] = (uint32_t)(word * 64 + bit);
            bits &= bits - 1;
        }
    }
    *candidates = count;
    for (size_t i = 0; i < n && count; ++i)
        if (dalw_refine_docs(rows->shard, terms[i], ids, &count) < 0) return -1;
    *matches = count;
    return 0;
}
