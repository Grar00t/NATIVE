#define _POSIX_C_SOURCE 200809L
#include "dalw.h"
#include "niyah_dalw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int decimal(const char *s, uint64_t limit, uint64_t *result)
{
    uint64_t value = 0;
    if (!*s) return 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return 0;
        unsigned digit = (unsigned)(*s - '0');
        if (value > (limit - digit) / 10) return 0;
        value = 10 * value + digit;
    }
    *result = value;
    return 1;
}

static int parse_row(char *line, DalwSrc *src)
{
    char *fields[7] = {line};
    uint64_t offset, length;
    for (size_t i = 1; i < 7; ++i) {
        char *tab = strchr(fields[i - 1], '\t');
        if (!tab) return 0;
        *tab = '\0';
        fields[i] = tab + 1;
    }
    if (!decimal(fields[3], UINT64_MAX, &offset) || !decimal(fields[4], UINT32_MAX, &length))
        return 0;
    src->url = fields[0];
    src->title = fields[1];
    src->warc_file = fields[2];
    src->warc_offset = offset;
    src->warc_len = (uint32_t)length;
    src->digest = fields[5];
    src->text = fields[6];
    src->text_len = strlen(fields[6]);
    return 1;
}

static int cmd_index(const char *out, const char *in, int skip_invalid)
{
    FILE *f = fopen(in, "rb");
    if (!f) { perror(in); return 1; }
    DalwBuilder *builder = dalw_builder_new();
    char *line = NULL;
    size_t cap = 0, ok = 0, bad = 0;
    ssize_t len;
    int rc = 1;
    const double start = now_ms();
    if (!builder) { fputs("cannot allocate index builder\n", stderr); goto done; }
    while ((len = getline(&line, &cap, f)) >= 0) {
        DalwSrc src;
        while (len && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = 0;
        if (memchr(line, '\0', (size_t)len) || !parse_row(line, &src)) {
            ++bad;
            fprintf(stderr, "%s:%zu: invalid TSV record\n", in, ok + bad);
            if (!skip_invalid) goto done;
            continue;
        }
        if (dalw_builder_add_ex(builder, &src) != 0) {
            fprintf(stderr, "%s:%zu: cannot add document\n", in, ok + bad + 1);
            goto done;
        }
        ++ok;
    }
    if (ferror(f) || !feof(f)) { perror(in); goto done; }
    if (fclose(f) != 0) { f = NULL; perror(in); goto done; }
    f = NULL;
    if (dalw_builder_write(builder, out) != 0) {
        fprintf(stderr, "cannot create index: %s\n", out);
        goto done;
    }
    fprintf(stderr, "indexed %zu docs (%zu skipped) -> %s in %.0f ms\n", ok, bad, out, now_ms() - start);
    rc = 0;
done:
    if (f) fclose(f);
    free(line);
    dalw_builder_free(builder);
    return rc;
}

static char *join_words(int count, char **words)
{
    size_t size = 0;
    for (int i = 0; i < count; ++i) {
        size_t n = strlen(words[i]);
        if (n >= SIZE_MAX - size) return NULL;
        size += n + 1;
    }
    char *query = malloc(size);
    if (!query) return NULL;
    char *next = query;
    for (int i = 0; i < count; ++i) {
        size_t n = strlen(words[i]);
        memcpy(next, words[i], n);
        next += n;
        *next++ = i + 1 == count ? '\0' : ' ';
    }
    return query;
}

static int cmd_search(char *paths_csv, size_t k, const char *query)
{
    const char *paths[NIYAH_DALW_MAX_SHARDS];
    size_t count = 0;
    char *path = paths_csv;
    while (path) {
        char *comma = strchr(path, ',');
        if (comma) *comma = '\0';
        if (!*path || count == NIYAH_DALW_MAX_SHARDS) {
            fputs("invalid shard list\n", stderr);
            return 1;
        }
        paths[count++] = path;
        path = comma ? comma + 1 : NULL;
    }

    int status = 0, rc = 1;
    const int exact = getenv("DALW_EXACT") != NULL;
    NiyahDalw *index = niyah_dalw_open(paths, count, getenv("DALW_VERIFY") != NULL, &status);
    if (!index) { fprintf(stderr, "%s\n", niyah_dalw_strerror(status)); return 1; }
    size_t docs = niyah_dalw_doc_count(index);
    size_t limit = k < docs ? k : docs;
    if (!limit) limit = 1;
    NiyahDalwHit *hits = limit <= SIZE_MAX / sizeof *hits ? malloc(limit * sizeof *hits) : NULL;
    char *receipt = NULL;
    size_t capacity = 0;
    if (!hits) { fputs("cannot allocate search results\n", stderr); goto done; }
    const double start = now_ms();
    size_t found = niyah_dalw_query(index, query, exact, hits, limit, &status);
    if (status != NIYAH_DALW_OK) {
        fprintf(stderr, "%s\n", niyah_dalw_strerror(status));
        if (status == NIYAH_DALW_E_CORRUPT) rc = 2;
        goto done;
    }
    for (size_t i = 0; i < found; ++i) {
        int n = niyah_dalw_receipt(index, &hits[i], query, exact, NULL, 0);
        if (n < 0) { fputs("invalid document metadata\n", stderr); goto done; }
        if ((size_t)n >= capacity) {
            char *next = realloc(receipt, (size_t)n + 1);
            if (!next) { fputs("cannot allocate result metadata\n", stderr); goto done; }
            receipt = next;
            capacity = (size_t)n + 1;
        }
        if (niyah_dalw_receipt(index, &hits[i], query, exact, receipt, capacity) != n) {
            fputs("cannot format search result\n", stderr); goto done;
        }
        if (puts(receipt) == EOF) { perror("stdout"); goto done; }
    }
    if (fflush(stdout) != 0) { perror("stdout"); goto done; }
    fprintf(stderr, "# %zu hits, %zu shard(s), %.3f ms\n", found, count, now_ms() - start);
    rc = 0;
done:
    free(receipt);
    free(hits);
    niyah_dalw_close(index);
    return rc;
}

static int cmd_info(const char *path, int verify) {
    DalwShard s; int r = dalw_open(&s, path, verify);
    if (r) { fprintf(stderr, "open failed (%d)%s\n", r, r == -2 ? " : SHA-256 MISMATCH" : ""); return 1; }
    printf("docs=%u terms=%u tokens=%llu bytes=%zu verified=%d sha256=", s.ndocs, s.nterms,
           (unsigned long long)s.total_tokens, s.size, s.signed_flag);
    for (int i = 0; i < 32; i++) printf("%02x", s.sha256[i]);
    puts(""); dalw_close(&s); return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "index")) {
        if (argc == 4) return cmd_index(argv[2], argv[3], 0);
        if (argc == 5 && !strcmp(argv[4], "--skip-invalid")) return cmd_index(argv[2], argv[3], 1);
    }
    if (argc >= 5 && !strcmp(argv[1], "search")) {
        uint64_t k;
        if (!decimal(argv[3], SIZE_MAX, &k) || k == 0) {
            fputs("K must be a positive decimal integer\n", stderr);
            return 1;
        }
        char *query = join_words(argc - 4, argv + 4);
        if (!query) { fputs("cannot allocate query\n", stderr); return 1; }
        int rc = cmd_search(argv[2], (size_t)k, query);
        free(query);
        return rc;
    }
    if (argc == 3 && !strcmp(argv[1], "info")) return cmd_info(argv[2], 0);
    if (argc == 3 && !strcmp(argv[1], "verify")) return cmd_info(argv[2], 1);
    fputs("usage:\n  dalw-cli index OUT.dalw IN.tsv [--skip-invalid]\n"
          "  dalw-cli search A.dalw[,B.dalw] K words...\n  dalw-cli info|verify SHARD\n"
          "env: DALW_EXACT=1 DALW_VERIFY=1 DALW_ISECT=scalar|avx2|avx512\n", stderr);
    return 1;
}
