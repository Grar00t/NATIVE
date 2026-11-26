#include "dalw_analysis.h"
#include <stdlib.h>

typedef struct { int64_t bit[64]; int seen; } Votes;
typedef struct { Votes *doc; uint32_t ndocs; } Corpus;
typedef struct { uint32_t index; uint16_t key; } BandEntry;

static unsigned log_weight(uint32_t n)
{
    unsigned weight = 1;
    while (n >>= 1) ++weight;
    return weight;
}

static int vote_block(uint64_t term, uint32_t df, const uint32_t *docs,
                      const uint32_t *tf, uint32_t count, void *context)
{
    Corpus *corpus = context;
    unsigned rarity = log_weight(corpus->ndocs / df);
    for (uint32_t i = 0; i < count; ++i) {
        Votes *v = &corpus->doc[docs[i]];
        int weight = (int)(rarity * log_weight(tf[i]));
        v->seen = 1;
        for (unsigned bit = 0; bit < 64; ++bit)
            v->bit[bit] += (term >> bit) & 1 ? weight : -weight;
    }
    return 0;
}

static int compare_band(const void *a, const void *b)
{
    const BandEntry *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return (x->index > y->index) - (x->index < y->index);
}

/* Values above the radius are never reported. The scalar path can stop early. */
static unsigned bounded_distance(uint64_t diff, unsigned radius)
{
    unsigned count = 0;
    while (diff && count <= radius) { diff &= diff - 1; ++count; }
    return count;
}

#if !defined(DALW_PORTABLE) && (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
#define DALW_RUNTIME_POPCNT 1
__attribute__((target("popcnt")))
static unsigned hardware_distance(uint64_t diff, unsigned radius)
{
    (void)radius;
    return (unsigned)__builtin_popcountll(diff);
}
#endif

/* At distance <= 3 at least one of four 16-bit bands must agree.
   Report each pair in the first agreeing band. */
static int scan_pairs(const DalwFingerprint *prints, size_t n, unsigned radius,
                      DalwPairFn report, void *context)
{
    if (n < 2) return 0;
    unsigned (*distance_of)(uint64_t, unsigned) = bounded_distance;
#ifdef DALW_RUNTIME_POPCNT
    if (__builtin_cpu_supports("popcnt")) distance_of = hardware_distance;
#endif
    BandEntry *band = malloc(n * sizeof *band);
    if (!band) return -1;
    int status = 0;
    for (unsigned b = 0; b < 4 && !status; ++b) {
        unsigned shift = 16 * b;
        for (size_t i = 0; i < n; ++i)
            band[i] = (BandEntry){(uint32_t)i, (uint16_t)(prints[i].hash >> shift)};
        qsort(band, n, sizeof *band, compare_band);
        for (size_t i = 0; i < n && !status; ++i) {
            const DalwFingerprint *left = &prints[band[i].index];
            for (size_t j = i + 1; j < n && band[i].key == band[j].key; ++j) {
                const DalwFingerprint *right = &prints[band[j].index];
                uint64_t diff = left->hash ^ right->hash;
                unsigned first = 0;
                while (first < b && (uint16_t)(diff >> (16 * first)) != 0) ++first;
                if (first != b) continue;
                unsigned distance = distance_of(diff, radius);
                if (distance <= radius && report(left->id, right->id, distance, context)) {
                    status = -1;
                    break;
                }
            }
        }
    }
    free(band);
    return status;
}

int dalw_fingerprints(const DalwShard *s, DalwFingerprint **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    if (!s->ndocs) return s->nterms ? -1 : 0;
    Corpus corpus = {calloc(s->ndocs, sizeof(Votes)), s->ndocs};
    if (!corpus.doc) return -1;
    DalwFingerprint *prints = NULL;
    int status = -1;
    if (dalw_walk_postings(s, vote_block, &corpus) < 0) goto done;
    prints = calloc(s->ndocs, sizeof *prints);
    if (!prints) goto done;
    size_t n = 0;
    for (uint32_t d = 0; d < s->ndocs; ++d) {
        const Votes *votes = &corpus.doc[d];
        if (!votes->seen) continue;
        uint64_t hash = 0;
        for (unsigned bit = 0; bit < 64; ++bit)
            if (votes->bit[bit] > 0) hash |= UINT64_C(1) << bit;
        prints[n++] = (DalwFingerprint){hash, d};
    }
    *out = prints;
    *count = n;
    prints = NULL;
    status = 0;
done:
    free(corpus.doc);
    free(prints);
    return status;
}

int dalw_simhash_pairs(const DalwShard *s, unsigned radius, DalwPairFn report, void *context)
{
    DalwFingerprint *prints;
    size_t count;
    if (radius > 3 || !report || dalw_fingerprints(s, &prints, &count)) return -1;
    int status = scan_pairs(prints, count, radius, report, context);
    free(prints);
    return status;
}
