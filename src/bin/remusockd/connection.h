#ifndef REMUSOCKD_CONNECTION_H
#define REMUSOCKD_CONNECTION_H

#include <stdint.h>

typedef struct Event Event;
typedef struct Connection Connection;

Connection *Connection_create(int fd);
Event *Connection_closed(Connection *self);
Event *Connection_dataReceived(Connection *self);
Event *Connection_dataSent(Connection *self);
int Connection_writeBuffer(Connection *self, char **buf, uint16_t *sz);
int Connection_commitWrite(Connection *self, uint16_t sz);
void Connection_destroy(Connection *self);

#endif
