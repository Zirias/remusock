#ifndef REMUSOCKD_SOCKCLIENT_H
#define REMUSOCKD_SOCKCLIENT_H

typedef struct Config Config;
typedef struct SockClient SockClient;

SockClient *SockClient_create(const Config *config);
void SockClient_destroy(SockClient *self);

#endif
