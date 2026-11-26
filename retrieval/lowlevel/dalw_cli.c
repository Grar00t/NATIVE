#define _POSIX_C_SOURCE 200809L
#include "dalw_analysis.h"
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

static int cmd_search(char *paths_csv, size_t k, const char *query, int any)
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
    const char *setting = getenv("DALW_EXACT");
    const int exact = setting && strcmp(setting, "0") != 0;
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
    size_t found = niyah_dalw_query_mode(index, query, exact, any, hits, limit, &status);
    if (status != NIYAH_DALW_OK) {
        fprintf(stderr, "%s\n", niyah_dalw_strerror(status));
        if (status == NIYAH_DALW_E_CORRUPT) rc = 2;
        goto done;
    }
    for (size_t i = 0; i < found; ++i) {
        int n = niyah_dalw_receipt_mode(index, &hits[i], query, exact, any, NULL, 0);
        if (n < 0) { fputs("invalid document metadata\n", stderr); goto done; }
        if ((size_t)n >= capacity) {
            char *next = realloc(receipt, (size_t)n + 1);
            if (!next) { fputs("cannot allocate result metadata\n", stderr); goto done; }
            receipt = next;
            capacity = (size_t)n + 1;
        }
        if (niyah_dalw_receipt_mode(index, &hits[i], query, exact, any, receipt, capacity) != n) {
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

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void digest_hex(const uint8_t digest[32], char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        hex[2*i] = digits[digest[i] >> 4];
        hex[2*i+1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
}

static int cmd_info(const char *path, int verify, const char *pin) {
    uint8_t expected[32];
    if (pin) {
        if (strlen(pin) != 64) { fputs("expected 64 hexadecimal digits\n", stderr); return 1; }
        for (size_t i = 0; i < sizeof expected; ++i) {
            int hi = hex_digit(pin[2 * i]), lo = hex_digit(pin[2 * i + 1]);
            if (hi < 0 || lo < 0) { fputs("invalid SHA-256 digit\n", stderr); return 1; }
            expected[i] = (uint8_t)(16 * hi + lo);
        }
    }
    DalwShard s; int r = dalw_open(&s, path, verify);
    if (r) { fprintf(stderr, "open failed (%d)%s\n", r, r == -2 ? " : SHA-256 MISMATCH" : ""); return 1; }
    if (pin && memcmp(s.sha256, expected, sizeof expected) != 0) {
        fputs("shard differs from the expected digest\n", stderr);
        dalw_close(&s);
        return 1;
    }
    printf("docs=%u terms=%u tokens=%llu bytes=%zu verified=%d sha256=", s.ndocs, s.nterms,
           (unsigned long long)s.total_tokens, s.size, s.signed_flag);
    char hex[65];
    digest_hex(s.sha256, hex);
    puts(hex); dalw_close(&s); return 0;
}

static int cmd_batch(const DalwShard *shard, int use_rows)
{
    DalwRows *rows = NULL;
    uint32_t *ids = calloc(shard->ndocs ? shard->ndocs : 1, sizeof *ids);
    char *line = NULL;
    size_t capacity = 0, queries = 0;
    uint64_t candidates = 0, matches = 0;
    int rc = 1;
    double start = now_ms();
    if (!ids || (use_rows && !(rows = dalw_rows_build(shard)))) {
        fputs("cannot build batch index\n", stderr);
        goto done;
    }
    double ready = now_ms();
    const char *setting = getenv("DALW_EXACT");
    int mode = setting && strcmp(setting, "0") ? DALW_Q_EXACT : DALW_Q_STEM;
    ssize_t len;
    while ((len = getline(&line, &capacity, stdin)) >= 0) {
        uint64_t terms[DALW_MAX_QUERY_TERMS];
        if (memchr(line, 0, (size_t)len)) { fputs("NUL byte in query\n", stderr); goto done; }
        size_t n = dalw_parse_query(line, mode, terms, DALW_MAX_QUERY_TERMS);
        size_t hit = 0, candidate = 0;
        int status = 0;
        if (!rows) hit = dalw_query(shard, terms, n, ids, shard->ndocs, &status);
        else status = dalw_rows_match(rows, terms, n, ids, &hit, &candidate);
        if (status < 0) { fputs("invalid posting list\n", stderr); goto done; }
        ++queries;
        candidates += candidate;
        matches += hit;
        if (putchar('[') == EOF) goto done;
        for (size_t i = 0; i < hit; ++i)
            if (printf("%s%u", i ? "," : "", ids[i]) < 0) goto done;
        if (puts("]") == EOF) goto done;
    }
    if (ferror(stdin) || !feof(stdin) || fflush(stdout)) goto done;
    fprintf(stderr, "queries=%zu matches=%llu setup_ms=%.3f batch_ms=%.3f",
            queries, (unsigned long long)matches, ready - start, now_ms() - ready);
    if (rows) fprintf(stderr, " row_bytes=%zu candidates=%llu",
                      dalw_rows_bytes(rows), (unsigned long long)candidates);
    fputc('\n', stderr);
    rc = 0;
done:
    free(line);
    free(ids);
    dalw_rows_free(rows);
    return rc;
}

static int print_pair(uint32_t first, uint32_t second, unsigned distance, void *context)
{
    (void)context;
    return printf("{\"first_doc\":%u,\"second_doc\":%u,\"hamming\":%u}\n",
                  first, second, distance) < 0;
}

static int cmd_fingerprints(const DalwShard *shard)
{
    DalwFingerprint *prints;
    size_t count;
    if (dalw_fingerprints(shard, &prints, &count)) {
        fputs("cannot read fingerprint features\n", stderr);
        return 1;
    }
    char sha[65];
    digest_hex(shard->sha256, sha);
    int rc = 1;
    if (printf("{\"format\":\"dalw-simhash-v1\",\"shard\":\"%s\",\"documents\":%u}\n",
               sha, shard->ndocs) < 0) goto done;
    size_t i = 0;
    for (uint32_t d = 0; d < shard->ndocs; ++d) {
        if (i < count && prints[i].id == d) {
            if (printf("{\"doc\":%u,\"simhash\":\"%016llx\"}\n", d,
                       (unsigned long long)prints[i].hash) < 0) goto done;
            ++i;
        } else if (printf("{\"doc\":%u,\"simhash\":null}\n", d) < 0) goto done;
    }
    rc = fflush(stdout) != 0;
done:
    free(prints);
    return rc;
}

static int cmd_cite(const DalwShard *shard, uint32_t id, const char *source, const char *quote)
{
    DalwDoc doc;
    size_t length = strlen(quote);
    if (dalw_doc(shard, id, &doc) || doc.digest_len != 64 ||
        !length || length > doc.warc_len || length > SIZE_MAX / sizeof(size_t) ||
        doc.warc_offset > (uint64_t)INT64_MAX - doc.warc_len) {
        fputs("invalid document, source range, digest, or quote length\n", stderr);
        return 1;
    }
    uint8_t expected[32];
    for (size_t i = 0; i < sizeof expected; ++i) {
        int hi = hex_digit(doc.digest[2*i]), lo = hex_digit(doc.digest[2*i+1]);
        if (hi < 0 || lo < 0) { fputs("invalid source SHA-256\n", stderr); return 1; }
        expected[i] = (uint8_t)(hi * 16 + lo);
    }
    FILE *f = fopen(source, "rb");
    if (!f) { perror(source); return 1; }
    int rc = 1;
    size_t *prefix = NULL;
    if (fseeko(f, (off_t)doc.warc_offset, SEEK_SET)) { perror(source); goto done; }
    prefix = calloc(length, sizeof *prefix);
    if (!prefix) { fputs("cannot allocate quote matcher\n", stderr); goto done; }
    for (size_t i = 1, k = 0; i < length; ++i) {
        while (k && quote[k] != quote[i]) k = prefix[k - 1];
        if (quote[k] == quote[i]) ++k;
        prefix[i] = k;
    }
    uint8_t buffer[8192], actual[32];
    uint64_t first = UINT64_MAX;
    size_t matched = 0;
    DalwSha256 hash;
    dalw_sha256_init(&hash);
    for (uint32_t read = 0; read < doc.warc_len;) {
        size_t need = doc.warc_len - read;
        if (need > sizeof buffer) need = sizeof buffer;
        if (fread(buffer, 1, need, f) != need) { fputs("incomplete source range\n", stderr); goto done; }
        dalw_sha256_update(&hash, buffer, need);
        for (size_t i = 0; first == UINT64_MAX && i < need; ++i) {
            while (matched && (uint8_t)quote[matched] != buffer[i]) matched = prefix[matched - 1];
            if ((uint8_t)quote[matched] == buffer[i]) ++matched;
            if (matched == length) first = doc.warc_offset + read + i + 1 - length;
        }
        read += (uint32_t)need;
    }
    dalw_sha256_final(&hash, actual);
    if (memcmp(actual, expected, sizeof actual)) { fputs("source digest mismatch\n", stderr); goto done; }
    if (first == UINT64_MAX) { fputs("quote absent from verified source range\n", stderr); goto done; }
    char sha[65];
    digest_hex(shard->sha256, sha);
    if (printf("{\"shard\":\"%s\",\"doc\":%u,\"source_sha256\":\"%.*s\","
               "\"source_offset\":%llu,\"source_length\":%u,\"offset\":%llu,\"length\":%zu}\n",
               sha, id, 64, doc.digest, (unsigned long long)doc.warc_offset, doc.warc_len,
               (unsigned long long)first, length) >= 0 && !fflush(stdout)) rc = 0;
done:
    free(prefix);
    fclose(f);
    return rc;
}

static int cmd_analysis(int argc, char **argv)
{
    uint64_t arg = 0;
    int batch = !strcmp(argv[1], "batch-match"), pairs = !strcmp(argv[1], "duplicates");
    int prints = !strcmp(argv[1], "fingerprints");
    if ((prints && argc != 3) || (batch && !(argc == 3 || (argc == 4 && !strcmp(argv[3], "--rows")))) ||
        (pairs && !(argc == 3 || (argc == 4 && decimal(argv[3], UINT32_MAX, &arg) && arg <= 3))) ||
        (!prints && !batch && !pairs && !(argc == 6 && decimal(argv[3], UINT32_MAX, &arg)))) {
        fputs("invalid analysis arguments\n", stderr);
        return 1;
    }
    DalwShard shard;
    if (dalw_open(&shard, argv[2], 1)) { fputs("cannot verify shard\n", stderr); return 1; }
    int rc;
    if (batch) rc = cmd_batch(&shard, argc == 4);
    else if (prints) rc = cmd_fingerprints(&shard);
    else if (pairs) {
        rc = dalw_simhash_pairs(&shard, argc == 3 ? 3 : (unsigned)arg, print_pair, NULL) || fflush(stdout);
        if (rc) fputs("cannot enumerate duplicate pairs\n", stderr);
    } else rc = cmd_cite(&shard, (uint32_t)arg, argv[4], argv[5]);
    dalw_close(&shard);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "batch-match") || !strcmp(argv[1], "duplicates") ||
                      !strcmp(argv[1], "fingerprints") || !strcmp(argv[1], "cite"))) return cmd_analysis(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "index")) {
        if (argc == 4) return cmd_index(argv[2], argv[3], 0);
        if (argc == 5 && !strcmp(argv[4], "--skip-invalid")) return cmd_index(argv[2], argv[3], 1);
    }
    if (argc >= 5 && (!strcmp(argv[1], "search") || !strcmp(argv[1], "search-any"))) {
        uint64_t k;
        if (!decimal(argv[3], SIZE_MAX, &k) || k == 0) {
            fputs("K must be a positive decimal integer\n", stderr);
            return 1;
        }
        char *query = join_words(argc - 4, argv + 4);
        if (!query) { fputs("cannot allocate query\n", stderr); return 1; }
        int rc = cmd_search(argv[2], (size_t)k, query, !strcmp(argv[1], "search-any"));
        free(query);
        return rc;
    }
    if (argc == 3 && !strcmp(argv[1], "info")) return cmd_info(argv[2], 0, NULL);
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "verify"))
        return cmd_info(argv[2], 1, argc == 4 ? argv[3] : NULL);
    fputs("usage:\n  dalw-cli index OUT.dalw IN.tsv [--skip-invalid]\n"
          "  dalw-cli search|search-any A.dalw[,B.dalw] K words...\n  dalw-cli info SHARD\n  dalw-cli verify SHARD [EXPECTED_BODY_SHA256]\n"
          "  dalw-cli batch-match SHARD [--rows] < queries.txt\n"
          "  dalw-cli fingerprints SHARD\n  dalw-cli duplicates SHARD [0..3]\n  dalw-cli cite SHARD DOC SOURCE QUOTE\n"
          "env: DALW_EXACT=1 DALW_VERIFY=1 DALW_ISECT=scalar|avx2|avx512\n", stderr);
    return 1;
}
