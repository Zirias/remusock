#ifndef REMUSOCKD_DAEMON_H
#define REMUSOCKD_DAEMON_H

typedef int (*daemon_main)(void *data);

int daemon_run(const daemon_main dmain, void *data, const char *pidfile);

#endif
