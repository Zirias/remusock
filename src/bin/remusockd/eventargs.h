#ifndef REMUSOCKD_EVENTARGS_H
#define REMUSOCKD_EVENTARGS_H

#include <stdint.h>

typedef struct DataReceivedEventArgs
{
    const char *buf;
    uint16_t size;
} DataReceivedEventArgs;

#endif
