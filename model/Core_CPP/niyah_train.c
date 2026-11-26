#include "niyah_core.h"
#include "niyah_train_full.h"
#include "tokenizer.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *open_training_data_path(const char *path)
{
    return fopen(path && path[0] ? path : "training.txt", "r");
}

static int parse_int(const char *s, int fallback)
{
    char *end = NULL;
    long value;
    if (!s || !s[0]) return fallback;
    errno = 0;
    value = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value <= 0 || value > INT32_MAX) {
        return fallback;
    }
    return (int)value;
}

static float parse_float(const char *s, float fallback)
{
    char *end = NULL;
    float value;
    if (!s || !s[0]) return fallback;
    errno = 0;
    value = strtof(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !isfinite(value) || value <= 0.0f) {
        return fallback;
    }
    return value;
}

static int parse_seed(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;
    if (!s || !isdigit((unsigned char)*s)) return 0;
    errno = 0;
    value = strtoull(s, &end, 0);
    if (errno || *end || value > UINT64_MAX) return 0;
    *out = (uint64_t)value;
    return 1;
}

/* Each nonblank physical line is one sample. Canonical LF keeps checkpoints
   independent of line ending or a missing final newline. */
static int next_sample(FILE *data, char **line, size_t *capacity)
{
    size_t used = 0;
    int c, nonblank = 0;
    while ((c = fgetc(data)) != EOF) {
        if (c == '\n') {
            if (nonblank) break;
            used = 0;
            continue;
        }
        if (c == '\0') { fputs("NUL byte in training input\n", stderr); return -1; }
        if (*capacity - used < 3) {
            if (*capacity > SIZE_MAX / 2) return -1;
            size_t next = *capacity ? *capacity * 2 : 4096;
            char *p = realloc(*line, next);
            if (!p) { fputs("cannot allocate training line\n", stderr); return -1; }
            *line = p;
            *capacity = next;
        }
        (*line)[used++] = (char)c;
        if (!isspace((unsigned char)c)) nonblank = 1;
    }
    if (ferror(data)) { perror("training input"); return -1; }
    if (!nonblank) return 0;
    if (used && (*line)[used - 1] == '\r') --used;
    (*line)[used++] = '\n';
    (*line)[used] = '\0';
    return 1;
}

static float cosine_lr(float base, float min_lr, uint32_t step,
                       uint32_t total, uint32_t warmup)
{
    if (step < warmup) {
        const float p = (float)step / (float)(warmup ? warmup : 1u);
        return min_lr + (base - min_lr) * p;
    }

    {
        const uint32_t denom = total > warmup ? total - warmup : 1u;
        float p = (float)(step - warmup) / (float)denom;
        if (p > 1.0f) p = 1.0f;
        return min_lr + (base - min_lr) * 0.5f
             * (1.0f + cosf(3.14159265f * p));
    }
}

int main(int argc, char **argv)
{
    const char *data_path = argc > 1 ? argv[1] : NULL;
    const int epochs = argc > 2 ? parse_int(argv[2], 0) : 5;
    const float base_lr = argc > 3 ? parse_float(argv[3], 0.0f) : 3e-4f;
    float min_lr = argc > 4 ? parse_float(argv[4], 0.0f) : 3e-5f;
    uint64_t seed = UINT64_C(0x4E49594148);
    uint32_t tokenizer_vocab;
    NiyahConfig cfg;
    NiyahModel *model = NULL;
    NiyahAdam *opt = NULL;
    FILE *data = NULL;
    char *line = NULL;
    size_t line_capacity = 0;
    int row;
    uint32_t total_lines = 0u;
    uint64_t step_budget;
    uint32_t total_steps;
    uint32_t warmup_steps;
    float ema = 0.0f;
    float best_ema = INFINITY;
    int bad_windows = 0;
    uint32_t global_step = 0u;
    clock_t t0 = clock();
    int rc = 0;
    int ep;

    if (argc > 6 || epochs <= 0 || base_lr <= 0.0f || min_lr <= 0.0f ||
        (argc > 5 && !parse_seed(argv[5], &seed))) {
        fputs("usage: niyah-train [file [epochs [learning_rate [min_rate [seed]]]]]\n", stderr);
        return 1;
    }
    if (min_lr > base_lr) min_lr = base_lr * 0.1f;

    tokenizer_init();
    tokenizer_vocab = tokenizer_vocab_size();
    if (tokenizer_vocab == 0u || tokenizer_vocab > NIYAH_MAX_VOCAB) {
        fputs("[NIYAH] invalid tokenizer vocabulary\n", stderr);
        tokenizer_free();
        return 1;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = NIYAH_MAGIC;
    cfg.version = NIYAH_VER;
    cfg.vocab_size = tokenizer_vocab;
    cfg.ctx_len = 64u;
    cfg.embed_dim = 128u;
    cfg.n_layers = 4u;
    cfg.n_heads = 8u;
    cfg.n_kv_heads = 8u;
    cfg.ffn_mult = 4u;
    cfg.rope_theta = 10000.0f;
    cfg.rms_eps = 1e-5f;

    model = niyah_alloc(&cfg);
    if (!model) {
        fputs("[NIYAH] alloc failed\n", stderr);
        tokenizer_free();
        return 1;
    }
    niyah_init_weights(model, seed);

    opt = niyah_adam_alloc(model);
    if (!opt) {
        fputs("[NIYAH] adam alloc failed\n", stderr);
        niyah_free(model);
        tokenizer_free();
        return 1;
    }
    opt->lr = base_lr;
    opt->beta1 = 0.9f;
    opt->beta2 = 0.999f;
    opt->eps = 1e-8f;
    opt->wd = 0.01f;

    data = open_training_data_path(data_path);
    if (!data) {
        fputs("[NIYAH] no data file found\n", stderr);
        niyah_adam_free(opt);
        niyah_free(model);
        tokenizer_free();
        return 1;
    }

    while ((row = next_sample(data, &line, &line_capacity)) > 0) {
        if (total_lines == UINT32_MAX) { rc = 1; goto cleanup; }
        ++total_lines;
    }
    if (row < 0 || total_lines == 0u) {
        if (row == 0) fputs("[NIYAH] no usable lines\n", stderr);
        rc = 1;
        goto cleanup;
    }

    step_budget = (uint64_t)total_lines * (uint64_t)epochs;
    if (step_budget > UINT32_MAX) {
        fputs("[NIYAH] invalid training step budget\n", stderr);
        rc = 1;
        goto cleanup;
    }
    total_steps = (uint32_t)step_budget;
    warmup_steps = total_steps / 20u;
    if (warmup_steps < 100u && total_steps > 100u) warmup_steps = 100u;
    if (warmup_steps >= total_steps && total_steps > 1u) warmup_steps = total_steps - 1u;

    printf("training_mode=full_parameter_detached_kv\n");
    printf("vocab_size=%u\n", cfg.vocab_size);
    printf("parameters=%zu\n", niyah_param_count(model));
    printf("seed=%llu\n", (unsigned long long)seed);
    fflush(stdout);

    t0 = clock();

    for (ep = 0; ep < epochs; ++ep) {
        float loss_sum = 0.0f;
        uint32_t steps = 0u;
        if (fseek(data, 0, SEEK_SET) != 0) { perror("training input"); rc = 1; goto cleanup; }

        while ((row = next_sample(data, &line, &line_capacity)) > 0) {
            uint32_t tokens[256];
            uint32_t n = tokenizer_encode(line, tokens, 256u);
            float loss;

            if (n < 2u) continue;
            if (n > cfg.ctx_len + 1u) n = cfg.ctx_len + 1u;

            opt->lr = cosine_lr(base_lr, min_lr, global_step,
                                total_steps, warmup_steps);
            loss = niyah_full_train_step(model, opt, tokens, n);
            if (!isfinite(loss)) {
                fputs("[NIYAH] non-finite loss\n", stderr);
                rc = 1;
                goto cleanup;
            }

            loss_sum += loss;
            ++steps;
            ++global_step;
            ema = (ema <= 0.0f) ? loss : 0.995f * ema + 0.005f * loss;

            if (steps % 100u == 0u) {
                printf("ep%d step%u loss=%.4f ema=%.4f lr=%.2e\n",
                       ep + 1, steps, (double)(loss_sum / (float)steps),
                       (double)ema, (double)opt->lr);
                fflush(stdout);
            }

            if (steps % 1000u == 0u) {
                if (ema < best_ema - 1e-3f) {
                    best_ema = ema;
                    bad_windows = 0;
                } else if (++bad_windows >= 6) {
                    goto cleanup;
                }
            }
        }
        if (row < 0) { rc = 1; goto cleanup; }
    }

cleanup:
    free(line);
    if (fclose(data) != 0) rc = 1;
    tokenizer_free();

    if (rc == 0 && niyah_save(model, "niyah_trained.bin") != 0) {
        fputs("[NIYAH] model save failed\n", stderr);
        rc = 1;
    }

    printf("training_steps=%u\n", global_step);
    printf("training_elapsed_s=%.3f\n",
           (double)(clock() - t0) / (double)CLOCKS_PER_SEC);
    niyah_adam_free(opt);
    niyah_free(model);
    return rc;
}
