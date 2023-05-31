#ifndef REMUSOCKD_PROTOCOL_H
#define REMUSOCKD_PROTOCOL_H

#include <stdint.h>

#define CMD_IDENT   0x49
#define CMD_PING    0x3f
#define CMD_PONG    0x21
#define CMD_HELLO   0x48
#define CMD_CONNECT 0x43
#define CMD_BYE	    0x42
#define	CMD_DATA    0x44

#define ARG_SERVER  0x53
#define ARG_CLIENT  0x43

#define IDENTTICKS  2

extern const uint8_t idsrv[];
extern const uint8_t idcli[];

typedef struct Protocol Protocol;

typedef struct PSC_Connection PSC_Connection;
typedef struct PSC_Server PSC_Server;
typedef struct PSC_UnixClientOpts PSC_UnixClientOpts;

Protocol *Protocol_create(PSC_Connection *tcp, PSC_Server *sockserver,
	PSC_UnixClientOpts *sockopts);
void Protocol_destroy(Protocol *self);

#endif
