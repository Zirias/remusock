#ifndef REMUSOCKD_EVENTARGS_H
#define REMUSOCKD_EVENTARGS_H

#include <stdint.h>

typedef struct DataReceivedEventArgs
{
    uint8_t *buf;
    int handling;
    uint16_t size;
    uint8_t offset;
} DataReceivedEventArgs;

#endif
