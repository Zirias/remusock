#ifndef REMUSOCKD_CONNECTION_H
#define REMUSOCKD_CONNECTION_H

#include "connopts.h"

#include <stdint.h>

typedef struct Connection Connection;
typedef struct Event Event;

Connection *Connection_create(int fd, ConnectionCreateMode mode);
Event *Connection_connected(Connection *self);
Event *Connection_closed(Connection *self);
Event *Connection_dataReceived(Connection *self);
Event *Connection_dataSent(Connection *self);
const char *Connection_remoteAddr(const Connection *self);
void Connection_setRemoteAddr(Connection *self, const char *addr);
int Connection_write(Connection *self, const char *buf, uint16_t sz, void *id);
void Connection_activate(Connection *self);
int Connection_confirmDataReceived(Connection *self);
void Connection_close(Connection *self);
void Connection_setData(Connection *self, void *data, void (*deleter)(void *));
void *Connection_data(const Connection *self);
void Connection_deleteLater(Connection *self);
void Connection_destroy(Connection *self);

#endif
