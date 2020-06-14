#ifndef REMUSOCKD_SYSLOG_H
#define REMUSOCKD_SYSLOG_H

#include <syslog.h>

void setSyslogLogger(int facility, int withStderr);

#endif

