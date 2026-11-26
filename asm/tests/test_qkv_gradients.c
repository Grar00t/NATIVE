/* Check the assembly SGD update against central differences of a separate
 * double-precision forward pass on the first training example. */
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DIM = 16, CTX = 8, VOCAB = 256, MATRIX = DIM * DIM };
/* Offsets in the exported little-endian binary32 parameter block. */
enum {
    EMBED = 0, POS = EMBED + VOCAB * DIM, FF = POS + CTX * DIM,
    FB = FF + MATRIX, OUT = FB + DIM, OB = OUT + VOCAB * DIM,
    WQ = OB + VOCAB, WK = WQ + MATRIX, WV = WK + MATRIX, COUNT = WV + MATRIX
};
_Static_assert(COUNT == 9616, "attention parameter count");
_Static_assert(CHAR_BIT == 8 && sizeof(float) == 4 && FLT_RADIX == 2 &&
               FLT_MANT_DIG == 24 && FLT_MIN_EXP == -125 && FLT_MAX_EXP == 128,
               "the attention export uses binary32");

struct example {
    unsigned char bytes[CTX + 1]; /* eight context bytes, then the target */
    bool allowed[VOCAB];
};

static bool read_example(const char *path, struct example *ex) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return false; }
    size_t count = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        ex->allowed[ch] = true;
        if (count < sizeof ex->bytes) ex->bytes[count++] = (unsigned char)ch;
    }
    bool ok = !ferror(f) && count == sizeof ex->bytes;
    if (fclose(f) != 0) ok = false;
    if (!ok) fprintf(stderr, "%s: expected a readable corpus of at least %d bytes\n", path, CTX + 1);
    return ok;
}

/* The training executable prints its held-out metric immediately before the
 * weights. Locate that boundary so a truncated or overlong export fails. */
static bool at_weights(FILE *f) {
    const char prefix[] = "heldout_loss_x1000 ";
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n')) return false;
        if (strncmp(line, prefix, sizeof prefix - 1) == 0) {
            const char *value = line + sizeof prefix - 1;
            size_t digits = strspn(value, "0123456789");
            return digits != 0 && strcmp(value + digits, "\n") == 0;
        }
    }
    return false;
}

static bool read_export(const char *path, double weights[COUNT]) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return false; }
    unsigned char raw[4 * COUNT];
    bool ok = at_weights(f) && fread(raw, 1, sizeof raw, f) == sizeof raw &&
              fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        fprintf(stderr, "%s: expected the held-out metric followed by exactly %zu weight bytes\n",
                path, sizeof raw);
        return false;
    }
    for (size_t i = 0; i < COUNT; ++i) {
        const unsigned char *b = &raw[4 * i];
        uint32_t bits = (uint32_t)b[0] | (uint32_t)b[1] << 8 |
                        (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        float value;
        memcpy(&value, &bits, sizeof value);
        if (!isfinite(value)) {
            fprintf(stderr, "%s: non-finite weight at index %zu\n", path, i);
            return false;
        }
        weights[i] = value;
    }
    return true;
}

static double dot(const double *a, const double *b) {
    double sum = 0.0;
    for (size_t i = 0; i < DIM; ++i) sum += a[i] * b[i];
    return sum;
}

static void project(const double *matrix, const double *input, double output[DIM]) {
    for (size_t i = 0; i < DIM; ++i) output[i] = dot(matrix + i * DIM, input);
}

static void softmax(double *values, size_t count) {
    double peak = values[0], sum = 0.0;
    for (size_t i = 1; i < count; ++i) peak = fmax(peak, values[i]);
    for (size_t i = 0; i < count; ++i) {
        values[i] = exp(values[i] - peak);
        sum += values[i];
    }
    for (size_t i = 0; i < count; ++i) values[i] /= sum;
}

static double loss(const struct example *ex, const double w[COUNT]) {
    double x[CTX][DIM], query[DIM], values[CTX][DIM], attention[CTX];
    double h[DIM], y[DIM], logits[VOCAB];
    for (size_t t = 0; t < CTX; ++t) {
        const size_t token_row = (size_t)ex->bytes[t] * DIM;
        for (size_t j = 0; j < DIM; ++j)
            x[t][j] = w[EMBED + token_row + j] + w[POS + t * DIM + j];
    }
    project(w + WQ, x[CTX - 1], query);
    for (size_t t = 0; t < CTX; ++t) {
        double key[DIM];
        project(w + WK, x[t], key);
        project(w + WV, x[t], values[t]);
        attention[t] = dot(query, key) / sqrt((double)DIM);
    }
    softmax(attention, CTX);
    for (size_t j = 0; j < DIM; ++j) {
        double sum = 0.0;
        for (size_t t = 0; t < CTX; ++t) sum += attention[t] * values[t][j];
        h[j] = x[CTX - 1][j] + sum;
    }
    for (size_t i = 0; i < DIM; ++i)
        y[i] = h[i] + fmax(0.0, w[FB + i] + dot(w + FF + i * DIM, h));

    size_t used = 0, target = 0;
    for (size_t b = 0; b < VOCAB; ++b) {
        if (!ex->allowed[b]) continue;
        if (b == ex->bytes[CTX]) target = used;
        logits[used++] = w[OB + b] + dot(w + OUT + b * DIM, y);
    }
    softmax(logits, used);
    return isfinite(logits[target]) ? -log(fmax(1e-30, logits[target])) : NAN;
}

static bool check_projection(const char *name, size_t offset, const struct example *ex,
                             double initial[COUNT], const double updated[COUNT]) {
    const double learning_rate = 0.025, epsilon = 0.0075;
    size_t best = 1; /* first off-diagonal element; first wins a tie */
    double delta = initial[offset + best] - updated[offset + best];
    for (size_t j = 0; j < MATRIX; ++j) {
        if (j / DIM == j % DIM) continue;
        double change = initial[offset + j] - updated[offset + j];
        if (fabs(change) > fabs(delta)) { best = j; delta = change; }
    }
    const size_t index = offset + best;
    double original = initial[index];
    initial[index] = original + epsilon;
    double plus = loss(ex, initial);
    initial[index] = original - epsilon;
    double minus = loss(ex, initial);
    initial[index] = original;

    double native = delta / learning_rate;
    double reference = (plus - minus) / (2.0 * epsilon);
    double relative = fabs(native - reference) / fmax(1e-12, fmax(fabs(native), fabs(reference)));
    printf("%s selected_offdiag %zu native_gradient %.17g reference_gradient %.17g "
           "relative_error %.17g native_step_delta %.17g\n", name, best, native, reference, relative, delta);
    if (!(fabs(delta) > 1e-10 && isfinite(relative) && relative < 0.08)) {
        fprintf(stderr, "%s: expected a nonzero update and relative error below 0.08\n", name);
        return false;
    }
    return true;
}

static double distance_from_identity(const double *matrix) {
    double largest = 0.0;
    for (size_t j = 0; j < MATRIX; ++j) {
        double expected = j / DIM == j % DIM ? 1.0 : 0.0;
        largest = fmax(largest, fabs(matrix[j] - expected));
    }
    return largest;
}

static const struct { const char *name; size_t offset; } projections[] = {
    {"WQ", WQ}, {"WK", WK}, {"WV", WV}
};

static bool check_trained(const char *path) {
    double weights[COUNT];
    if (!read_export(path, weights)) return false;
    for (size_t i = 0; i < sizeof projections / sizeof projections[0]; ++i) {
        if (distance_from_identity(weights + projections[i].offset) <= 1e-6) {
            fprintf(stderr, "%s: trained projection stayed at identity\n", projections[i].name);
            return false;
        }
    }
    puts("PASS: all three trained projections moved from identity");
    return true;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--trained") == 0)
        return check_trained(argv[2]) ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc != 4) {
        fprintf(stderr, "usage: %s CORPUS INITIAL_EXPORT ONE_STEP_EXPORT\n"
                        "       %s --trained TRAINED_EXPORT\n", argv[0], argv[0]);
        return EXIT_FAILURE;
    }
    struct example ex = {0};
    double initial[COUNT], updated[COUNT];
    if (!read_example(argv[1], &ex) || !read_export(argv[2], initial) ||
        !read_export(argv[3], updated)) return EXIT_FAILURE;
    for (size_t i = 0; i < sizeof projections / sizeof projections[0]; ++i) {
        const char *name = projections[i].name;
        size_t offset = projections[i].offset;
        if (distance_from_identity(initial + offset) != 0.0) {
            fprintf(stderr, "%s: initial projection differs from identity\n", name);
            return EXIT_FAILURE;
        }
        if (!check_projection(name, offset, &ex, initial, updated)) return EXIT_FAILURE;
    }

    double before = loss(&ex, initial), after = loss(&ex, updated);
    printf("LOSS one-step before %.17g after %.17g\n", before, after);
    if (!isfinite(before) || !isfinite(after) || !(after < before)) {
        fputs("one SGD step did not reduce the example loss\n", stderr);
        return EXIT_FAILURE;
    }
    puts("PASS: independently checked WQ/WK/WV one-step SGD gradients");
    return EXIT_SUCCESS;
}
