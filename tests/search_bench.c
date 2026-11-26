#define _POSIX_C_SOURCE 200809L
#include "niyah_index.h"
#include "dalw.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { DOCUMENTS = 3072, SAMPLES = 11 };
struct Workload { const char *label, *text; size_t capacity, repeats; };
static const struct Workload cases[] = {
    {"dense", "common", 10, 3}, {"sparse", "rare", 10, 100},
    {"missing", "unknownterm", 10, 500}, {"mixed", "شاهد common", 10, 3},
    {"large-k", "common", 512, 3}, {"duplicate", "شاهد شاهد", 64, 10},
    {"one", "word3071", 1, 100}
};

static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static int compare_ns(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static uint64_t run_query(const NiyahInvertedIndex *index, const DalwShard *shard,
                          const struct Workload *q, size_t cap, int dump) {
    if (index) {
        NiyahSearchHit hits[DOCUMENTS];
        size_t n = niyah_index_search(index, q->text, hits, cap);
        if (dump) {
            printf("memory %s %zu %zu\n", q->label, cap, n);
            for (size_t i = 0; i < n; ++i)
                printf("%016" PRIx64 " %a\n", hits[i].document_id, hits[i].score);
        }
        return n ? hits[0].document_id ^ n : 0;
    }
    uint64_t terms[32];
    DalwHit hits[DOCUMENTS];
    size_t nt = dalw_parse_query(q->text, DALW_Q_EXACT, terms, 32);
    int status;
    size_t n = dalw_search(shard, terms, nt, hits, cap, &status);
    if (status || !nt) { fprintf(stderr, "shard query failed: %s\n", q->label); exit(1); }
    if (dump) {
        printf("shard %s %zu %zu\n", q->label, cap, n);
        for (size_t i = 0; i < n; ++i)
            printf("%" PRIu32 " %a\n", hits[i].doc, (double)hits[i].score);
        if (n) {
            char receipt[2048];
            if (dalw_receipt(shard, hits, terms, nt, receipt, sizeof(receipt)) < 0) exit(1);
            puts(receipt);
        }
    }
    return n ? hits[0].doc ^ n : 0;
}

static void measure(const NiyahInvertedIndex *index, const DalwShard *shard, int dump) {
    const size_t limits[] = {1, 2, 10, 64, 512, DOCUMENTS};
    volatile uint64_t checksum = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const struct Workload *q = &cases[i];
        if (dump) {
            for (size_t j = 0; j < sizeof(limits) / sizeof(limits[0]); ++j)
                run_query(index, shard, q, limits[j], 1);
            continue;
        }
        uint64_t samples[SAMPLES];
        checksum ^= run_query(index, shard, q, q->capacity, 0);
        for (size_t j = 0; j < SAMPLES; ++j) {
            uint64_t start = now_ns();
            for (size_t r = 0; r < q->repeats; ++r)
                checksum ^= run_query(index, shard, q, q->capacity, 0);
            samples[j] = (now_ns() - start) / q->repeats;
        }
        qsort(samples, SAMPLES, sizeof(*samples), compare_ns);
        printf("%s %-10s k=%zu batch=%zu median_ns=%" PRIu64 "\n",
               index ? "memory" : "shard", q->label, q->capacity, q->repeats, samples[SAMPLES / 2]);
    }
    (void)checksum;
}

int main(int argc, char **argv) {
    int dump = argc == 2 && !strcmp(argv[1], "--dump");
    if (argc != 1 && !dump) { fprintf(stderr, "usage: %s [--dump]\n", argv[0]); return 2; }
    char dir[] = "/tmp/niyah-search-XXXXXX", path[96] = "";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(path, sizeof(path), "%s/test.dalw", dir);
    int rc = 1;
    DalwShard shard = {0};
    NiyahInvertedIndex index;
    NiyahDocument d = {0};
    niyah_index_init(&index, 1.2, 0.75);
    DalwBuilder *builder = dalw_builder_new();
    if (!builder) goto done;
    for (size_t i = 0; i < DOCUMENTS; ++i) {
        d.document_id = UINT64_MAX - UINT64_C(0x9e3779b97f4a7c15) * i;
        snprintf(d.title, sizeof(d.title), "وثيقة %zu", i);
        snprintf(d.url, sizeof(d.url), "local:doc/%zu", i);
        snprintf(d.text, sizeof(d.text), "common word%zu %s %s %s", i,
                 i % 257 == 0 ? "rare" : "other", i % 7 == 0 ? "شاهد" : "text",
                 i % 3 == 0 ? "common common" : "");
        DalwSrc src = {.title = d.title, .url = d.url, .text = d.text, .text_len = strlen(d.text)};
        if (!niyah_index_add_document(&index, &d) || dalw_builder_add_ex(builder, &src)) goto done;
    }
    if (dalw_builder_write(builder, path) || dalw_open(&shard, path, 1)) goto done;
    if (!dump) printf("documents=%d samples=%d\n", DOCUMENTS, SAMPLES);
    measure(&index, NULL, dump);
    measure(NULL, &shard, dump);
    rc = 0;
done:
    if (rc) fputs("search benchmark fixture failed\n", stderr);
    dalw_close(&shard);
    dalw_builder_free(builder);
    niyah_index_free(&index);
    unlink(path);
    rmdir(dir);
    return rc;
}
