#ifndef NIYAH_UTF8_H
#define NIYAH_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Decode one scalar. On an invalid sequence, consume one byte and return U+FFFD. */
static inline uint32_t niyah_utf8_next(const unsigned char *s, size_t len, size_t *i) {
    unsigned c = s[*i]; size_t n = len - *i;
    if (c < 0x80) { (*i)++; return c; }
    if (c >= 0xC2 && c <= 0xDF && n >= 2 && (s[*i+1] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x1Fu) << 6) | (s[*i+1] & 0x3Fu); *i += 2; return cp;
    }
    if (c >= 0xE0 && c <= 0xEF && n >= 3 && (s[*i+1] & 0xC0) == 0x80 && (s[*i+2] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x0Fu) << 12) | ((s[*i+1] & 0x3Fu) << 6) | (s[*i+2] & 0x3Fu);
        if (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) { *i += 3; return cp; }
    }
    if (c >= 0xF0 && c <= 0xF4 && n >= 4 && (s[*i+1] & 0xC0) == 0x80 &&
        (s[*i+2] & 0xC0) == 0x80 && (s[*i+3] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x07u) << 18) | ((s[*i+1] & 0x3Fu) << 12) |
                      ((s[*i+2] & 0x3Fu) << 6) | (s[*i+3] & 0x3Fu);
        if (cp >= 0x10000 && cp <= 0x10FFFF) { *i += 4; return cp; }
    }
    (*i)++; return 0xFFFD;
}

#endif
