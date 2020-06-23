#ifndef REMUSOCKD_SYSLOG_H
#define REMUSOCKD_SYSLOG_H

#include <syslog.h>

void setSyslogLogger(const char *ident, int facility, int withStderr);

#endif

