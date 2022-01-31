#ifndef REMUSOCKD_CONNECTION_H
#define REMUSOCKD_CONNECTION_H

#include "connopts.h"

#include <stdint.h>
#include <sys/socket.h>

typedef struct Connection Connection;
typedef struct Event Event;

typedef struct DataReceivedEventArgs
{
    uint8_t *buf;
    int handling;
    uint16_t size;
    uint8_t offset;
} DataReceivedEventArgs;

Connection *Connection_create(int fd, ConnectionCreateMode mode,
	uint8_t readOffset);
Event *Connection_connected(Connection *self);
Event *Connection_closed(Connection *self);
Event *Connection_dataReceived(Connection *self);
Event *Connection_dataSent(Connection *self);
const char *Connection_remoteAddr(const Connection *self);
const char *Connection_remoteHost(const Connection *self);
void Connection_setRemoteAddr(Connection *self,
	struct sockaddr *addr, socklen_t addrlen, int numericOnly);
void Connection_setRemoteAddrStr(Connection *self, const char *addr);
int Connection_write(Connection *self,
	const uint8_t *buf, uint16_t sz, void *id);
void Connection_activate(Connection *self);
int Connection_confirmDataReceived(Connection *self);
void Connection_close(Connection *self);
void Connection_setData(Connection *self, void *data, void (*deleter)(void *));
void *Connection_data(const Connection *self);
void Connection_destroy(Connection *self);

#endif
