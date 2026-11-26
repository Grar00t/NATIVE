#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define terminal _isatty
#define read_bytes _read
#define write_bytes _write
static uint64_t milliseconds(void) { return GetTickCount64(); }
static void pause_frame(void) { Sleep(400); }
#else
#include <time.h>
#include <unistd.h>
#define terminal isatty
#define read_bytes read
#define write_bytes write
static uint64_t milliseconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void pause_frame(void)
{
    struct timespec remaining = {0, 400000000};
    while (nanosleep(&remaining, &remaining) && errno == EINTR) {}
}
#endif

static int demo(void)
{
    static const char *dots[] = {"   ", ".  ", ".. ", "..."};
    if (!terminal(2)) return fputs("thinking...\n", stderr) == EOF;
    fputs("Display demo. Press Ctrl+C to stop.\n", stderr);
    for (unsigned frame = 0;; ++frame) {
        if (fprintf(stderr, "\rthinking%s", dots[frame % 4]) < 0 || fflush(stderr)) return 1;
        pause_frame();
    }
}

int main(int argc, char **argv)
{
    int count = argc == 2 && !strcmp(argv[1], "--count");
    if ((argc == 2 && !strcmp(argv[1], "--demo")) ||
        (argc == 1 && terminal(0) && terminal(1) && terminal(2))) return demo();
    if (argc != 1 && !count) {
        fputs("usage: think [--count | --demo]\n", stderr);
        return 2;
    }
#ifdef _WIN32
    if (_setmode(0, _O_BINARY) == -1 || _setmode(1, _O_BINARY) == -1) {
        perror("binary stream");
        return 1;
    }
#endif
    unsigned char buffer[32768];
    uint64_t bytes = 0, last = 0;
    int live = terminal(2);
    for (;;) {
        int n = (int)read_bytes(0, buffer, sizeof buffer);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("stdin");
            return 1;
        }
        for (unsigned sent = 0; sent < (unsigned)n;) {
            int written = (int)write_bytes(1, buffer + sent, (unsigned)n - sent);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { perror("stdout"); return 1; }
            sent += (unsigned)written;
        }
        bytes += (unsigned)n;
        if (live) {
            uint64_t now = milliseconds();
            if (now - last >= 400) {
                fprintf(stderr, "\r%" PRIu64 " bytes", bytes);
                fflush(stderr);
                last = now;
            }
        }
    }
    if (live || count) fprintf(stderr, "%s%" PRIu64 " bytes\n", live ? "\r" : "", bytes);
    return 0;
}
