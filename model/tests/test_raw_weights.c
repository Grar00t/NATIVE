#include "niyah_core.h"
#include "niyah_publish_internal.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "raw weights: line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static const char *fault_point;
static int fail_publication(const char *step) { return strcmp(step, fault_point) == 0; }

static int same_weights(const NiyahModel *a, const NiyahModel *b) {
    size_t n = niyah_param_count(a);
    return n && n == niyah_param_count(b) && memcmp(a->_pool, b->_pool, n * sizeof(float)) == 0;
}

int main(void) {
    const char *path = "raw-weights.bin";
    NiyahConfig cfg = { .embed_dim = 8, .n_heads = 2, .n_kv_heads = 1, .n_layers = 1,
                        .ffn_mult = 2, .vocab_size = 16, .ctx_len = 4,
                        .rope_theta = 10000, .rms_eps = 1e-5f };
    NiyahModel *a = niyah_alloc(&cfg), *b = niyah_alloc(&cfg), *loaded = NULL;
    CHECK(a && b);
    for (size_t i = 0; i < niyah_param_count(a); ++i) ((float *)b->_pool)[i] = (float)(i + 1);
    CHECK(niyah_save(a, path) == 0);

    const char *points[] = {"partial_write", "publish"};
    for (size_t i = 0; i < sizeof(points) / sizeof(*points); ++i) {
        fault_point = points[i];
        niyah_publish_set_fault(fail_publication);
        CHECK(niyah_save(b, path) == -1);
        niyah_publish_set_fault(NULL);
        CHECK(niyah_load(&loaded, path) == 0 && same_weights(a, loaded));
        niyah_free(loaded);
        loaded = NULL;
    }

#ifndef _WIN32
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        for (int i = 0; i < 16; ++i) if (niyah_save(i % 2 ? a : b, path)) _exit(1);
        _exit(0);
    }
    for (int i = 0; i < 24; ++i) {
        CHECK(niyah_load(&loaded, path) == 0);
        CHECK(same_weights(a, loaded) || same_weights(b, loaded));
        niyah_free(loaded);
        loaded = NULL;
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif

    CHECK(niyah_save(b, path) == 0);
    CHECK(niyah_load(&loaded, path) == 0 && same_weights(b, loaded));
    niyah_free(loaded);
    loaded = NULL;
    FILE *file = fopen(path, "ab");
    CHECK(file && fputc(1, file) != EOF && fclose(file) == 0);
    CHECK(niyah_load(&loaded, path) == -1 && !loaded);
    cfg.magic = NIYAH_MAGIC;
    cfg.version = NIYAH_VER;
    file = fopen(path, "wb");
    CHECK(file && fwrite(&cfg, sizeof(cfg), 1, file) == 1 && fclose(file) == 0);
    CHECK(niyah_load(&loaded, path) == -1 && !loaded);
    cfg.version += 1;
    file = fopen(path, "wb");
    CHECK(file && fwrite(&cfg, sizeof(cfg), 1, file) == 1 && fclose(file) == 0);
    CHECK(niyah_load(&loaded, path) == -2 && !loaded);
    cfg.ffn_mult = UINT32_MAX;
    CHECK(niyah_alloc(&cfg) == NULL);

    for (uint32_t k = 0; k < a->cfg.embed_dim; ++k) {
        a->token_embed[k] = k & 1 ? -FLT_MAX : FLT_MAX;
        a->rms_final[k] = 1.0f;
    }
    a->lm_head[0] = 1.0f;
    a->lm_head[a->cfg.embed_dim + 1] = 1.0f;
    const float *logits = niyah_forward(a, 0, 0);
    CHECK(logits && fabsf(logits[0] - 1.0f) < 1e-6f && fabsf(logits[1] + 1.0f) < 1e-6f);
    niyah_free(a);
    niyah_free(b);
    CHECK(remove(path) == 0);
    puts("Raw weight I/O and normalization passed");
    return 0;
}
