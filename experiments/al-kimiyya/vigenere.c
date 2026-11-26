#include <stdio.h>
#include <string.h>

static int in_alphabet(const char *s) {
    for (; *s; ++s) {
        if (*s < 'A' || *s > 'Z') return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1], "encrypt") && strcmp(argv[1], "decrypt"))) {
        fputs("usage: al-kimiyya encrypt|decrypt KEY TEXT\n", stderr);
        return 2;
    }
    const char *key = argv[2];
    const char *input = argv[3];
    if (!*key || !in_alphabet(key) || !in_alphabet(input)) {
        fputs("use uppercase ASCII letters and a nonempty key\n", stderr);
        return 2;
    }
    const int decrypt = strcmp(argv[1], "decrypt") == 0;
    size_t k = 0;
    for (; *input; ++input) {
        int shift = key[k] - 'A';
        int value = *input - 'A' + (decrypt ? 26 - shift : shift);
        if (putchar('A' + value % 26) == EOF) return 1;
        if (key[++k] == '\0') k = 0;
    }
    return putchar('\n') == EOF || fflush(stdout) == EOF;
}
