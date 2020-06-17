#ifndef REMUSOCKD_EVENTARGS_H
#define REMUSOCKD_EVENTARGS_H

#include <stdint.h>

typedef struct Connection Connection;

typedef struct ClientConnectionEventArgs
{
    Connection *client;
} ClientConnectionEventArgs;

typedef struct DataReceivedEventArgs
{
    const char *buf;
    int handling;
    uint16_t size;
} DataReceivedEventArgs;

typedef struct DataSentEventArgs
{
    const void *buf;
} DataSentEventArgs;

#endif
