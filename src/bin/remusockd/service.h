#ifndef REMUSOCKD_SERVICE_H
#define REMUSOCKD_SERVICE_H

typedef struct Config Config;
typedef struct Event Event;

int Service_init(const Config *config);
Event *Service_readyRead(void);
Event *Service_readyWrite(void);
Event *Service_shutdown(void);
void Service_registerRead(int id);
void Service_unregisterRead(int id);
void Service_registerWrite(int id);
void Service_unregisterWrite(int id);
int Service_run(void);
void Service_quit(void);
void Service_done(void);

#endif
