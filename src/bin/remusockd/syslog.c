#include "syslog.h"
#include "log.h"

static int syslogLevels[] =
{
    LOG_CRIT,
    LOG_ERR,
    LOG_WARNING,
    LOG_INFO,
    LOG_DEBUG
};

static void writeSyslog(LogLevel level, const char *message, void *data)
{
    (void)data;
    syslog(syslogLevels[level], "%s", message);
}

void setSyslogLogger(const char *ident, int facility, int withStderr)
{
    int logopts = LOG_PID;
    if (withStderr) logopts |= LOG_PERROR;
    openlog(ident, logopts, facility);
    setCustomLogger(writeSyslog, 0);
}

