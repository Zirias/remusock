#include "daemon.h"
#include "log.h"

#include <stdio.h>
#include <unistd.h>

int dmain(void *data)
{
    (void)data;

    while(!sleep(1));
    return 0;
}

int main(void)
{
    setFileLogger(stderr);
    return daemon_run(dmain, 0, "/tmp/remusockd.pid");
}

