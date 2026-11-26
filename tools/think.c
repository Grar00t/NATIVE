#define _POSIX_C_SOURCE 200809L
#include "think.h"
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
static int milliseconds(uint64_t *value) { *value = GetTickCount64(); return 0; }
static int pause_ms(uint32_t delay) { Sleep(delay); return 0; }
#else
#include <time.h>
#include <unistd.h>
#define terminal isatty
#define read_bytes read
#define write_bytes write
static int milliseconds(uint64_t *value)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    *value = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
    return 0;
}
static int pause_ms(uint32_t delay)
{
    struct timespec remaining = {delay / 1000, (long)(delay % 1000) * 1000000};
    while (nanosleep(&remaining, &remaining))
        if (errno != EINTR) return -1;
    return 0;
}
#endif

/* Caller owns both callbacks. A poll must return promptly. */
int think_run(const ThinkTask *task, const ThinkConnector *connector)
{
    uint64_t start, now;
    int result = THINK_ERROR;
    const char *event = "failed";
    if (!task || !task->id || !*task->id || !task->poll_ms || !task->timeout_ms ||
        task->poll_ms == UINT32_MAX || !connector || !connector->poll || !connector->report)
        return THINK_USAGE;
    if (milliseconds(&start) || connector->report(connector->context, task, "started"))
        return THINK_ERROR;
    for (;;) {
        if (milliseconds(&now)) break;
        if (now - start >= task->timeout_ms) { result = THINK_TIMEOUT; break; }
        ThinkState state = connector->poll(connector->context, task);
        if (state == THINK_FAILED || milliseconds(&now)) break;
        uint64_t elapsed = now - start;
        if (elapsed >= task->timeout_ms) { result = THINK_TIMEOUT; break; }
        if (state == THINK_READY) { result = THINK_OK; break; }
        uint32_t left = task->timeout_ms - (uint32_t)elapsed;
        if (pause_ms(task->poll_ms < left ? task->poll_ms : left)) break;
    }
    if (result == THINK_OK) event = "completed";
    if (result == THINK_TIMEOUT) event = "timed_out";
    return connector->report(connector->context, task, event) ? THINK_ERROR : result;
}

/* Historical terminal refresh interval. Task polling is set per invocation. */
enum { meter_refresh_ms = 400 };

static int demo(void)
{
    static const char *dots[] = {"   ", ".  ", ".. ", "..."};
    if (!terminal(2)) return fputs("thinking...\n", stderr) == EOF;
    for (unsigned frame = 0; frame < sizeof dots / sizeof dots[0]; ++frame) {
        if (fprintf(stderr, "\rthinking%s", dots[frame]) < 0 || fflush(stderr)) return 1;
        if (pause_ms(meter_refresh_ms)) return 1;
    }
    return fputc('\n', stderr) == EOF;
}

int think_cli(int argc, char **argv)
{
    int count = argc == 2 && !strcmp(argv[1], "--count");
#ifdef THINK_WITH_AGENTS
    if (argc >= 2 && !strcmp(argv[1], "run")) return agent_cli(argc - 1, argv + 1);
#endif
    if (argc == 2 && !strcmp(argv[1], "--demo")) return demo();
    if ((argc != 1 && !count) || (argc == 1 && terminal(0) && terminal(1))) {
        fputs("usage: think [--count | --demo]"
#ifdef THINK_WITH_AGENTS
              " | think run --help"
#endif
              "\n", stderr);
        if (argc == 2 && !strcmp(argv[1], "--help")) return 0;
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
            uint64_t now;
            if (!milliseconds(&now) && now - last >= meter_refresh_ms) {
                fprintf(stderr, "\r%" PRIu64 " bytes", bytes);
                fflush(stderr);
                last = now;
            }
        }
    }
    if (live || count) fprintf(stderr, "%s%" PRIu64 " bytes\n", live ? "\r" : "", bytes);
    return 0;
}

#if !defined(_WIN32) || !defined(THINK_WITH_AGENTS)
int main(int argc, char **argv) { return think_cli(argc, argv); }
#endif
