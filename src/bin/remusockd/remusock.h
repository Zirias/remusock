#ifndef REMUSOCKD_REMUSOCK_H
#define REMUSOCKD_REMUSOCK_H

typedef struct Config Config;

int RemUSock_init(const Config *config);
void RemUSock_done(void);

#endif
