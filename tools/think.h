#ifndef NATIVE_THINK_H
#define NATIVE_THINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;
    uint32_t timeout_ms;
    uint32_t poll_ms;
} ThinkTask;

typedef enum { THINK_WAIT, THINK_READY, THINK_FAILED } ThinkState;
typedef struct {
    void *context;
    ThinkState (*poll)(void *context, const ThinkTask *task);
    int (*report)(void *context, const ThinkTask *task, const char *event);
} ThinkConnector;

enum { THINK_OK = 0, THINK_ERROR = 1, THINK_USAGE = 2, THINK_TIMEOUT = 124 };
int think_run(const ThinkTask *task, const ThinkConnector *connector);
int think_cli(int argc, char **argv);
int agent_cli(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
