#ifndef REMUSOCKD_SOCKSERVER_H
#define REMUSOCKD_SOCKSERVER_H

typedef struct Config Config;
typedef struct SockServer SockServer;

SockServer *SockServer_create(const Config *config);
void SockServer_destroy(SockServer *self);

#endif
