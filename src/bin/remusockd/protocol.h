#ifndef REMUSOCKD_PROTOCOL_H
#define REMUSOCKD_PROTOCOL_H

typedef struct Config Config;

int Protocol_init(const Config *config);
int Protocol_done(void);

#endif
