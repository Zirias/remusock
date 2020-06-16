#ifndef REMUSOCKD_EVENTARGS_H
#define REMUSOCKD_EVENTARGS_H

#include <stdint.h>

typedef struct DataReceivedEventArgs
{
    const char *buf;
    int handling;
    uint16_t size;
} DataReceivedEventArgs;

#endif
