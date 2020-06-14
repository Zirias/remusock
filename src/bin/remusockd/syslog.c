#include "syslog.h"
#include "log.h"

typedef struct
{
    int facility;
    int withStderr;
} SyslogOpts;

SyslogOpts opts;

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
    SyslogOpts *so = data;
    syslog(so->facility | syslogLevels[level], "%s", message);
    if (so->withStderr) writeFile(level, message, stderr);
}

void setSyslogLogger(int facility, int withStderr)
{
    opts.facility = facility;
    opts.withStderr = withStderr;
    setCustomLogger(writeSyslog, &opts);
}
