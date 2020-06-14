#include "daemon.h"
#include "syslog.h"

#include <unistd.h>

int dmain(void *data)
{
    (void)data;

    while(!sleep(1));
    return 0;
}

int main(void)
{
    setSyslogLogger(LOG_DAEMON, 1);
    return daemon_run(dmain, 0, "/tmp/remusockd.pid");
}

