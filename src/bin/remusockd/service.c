#include "service.h"
#include "config.h"
#include "event.h"
#include "log.h"

#include <signal.h>
#include <string.h>
#include <sys/select.h>

static const Config *cfg;
static Event *readyRead;
static Event *readyWrite;
static Event *shutdown;

static fd_set readfds;
static fd_set writefds;
static int nread;
static int nwrite;
static int nfds;

static sig_atomic_t shutdownRequest;

static void handlesig(int signum)
{
    (void)signum;
    shutdownRequest = 1;
}

static void tryReduceNfds(int id)
{
    if (!nread && !nwrite)
    {
	nfds = 0;
    }
    else if (id+1 >= nfds)
    {
	int fd;
	for (fd = id; fd >= 0; --fd)
	{
	    if (FD_ISSET(fd, &readfds) || FD_ISSET(fd, &writefds))
	    {
		break;
	    }
	}
	nfds = fd+1;
    }
}

int Service_init(const Config *config)
{
    if (cfg) return -1;
    cfg = config;
    readyRead = Event_create(0);
    readyWrite = Event_create(0);
    shutdown = Event_create(0);
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    nread = 0;
    nwrite = 0;
    nfds = 0;
    shutdownRequest = 0;
    return 0;
}

Event *Service_readyRead(void)
{
    return readyRead;
}

Event *Service_readyWrite(void)
{
    return readyWrite;
}

Event *Service_shutdown(void)
{
    return shutdown;
}

void Service_registerRead(int id)
{
    if (FD_ISSET(id, &readfds)) return;
    FD_SET(id, &readfds);
    ++nread;
    if (id >= nfds) nfds = id+1;
}

void Service_unregisterRead(int id)
{
    if (!FD_ISSET(id, &readfds)) return;
    FD_CLR(id, &readfds);
    --nread;
    tryReduceNfds(id);
}

void Service_registerWrite(int id)
{
    if (FD_ISSET(id, &writefds)) return;
    FD_SET(id, &writefds);
    ++nwrite;
    if (id >= nfds) nfds = id+1;
}

void Service_unregisterWrite(int id)
{
    if (!FD_ISSET(id, &writefds)) return;
    FD_CLR(id, &writefds);
    --nwrite;
    tryReduceNfds(id);
}

int Service_run(void)
{
    if (!cfg) return -1;

    struct sigaction handler;
    memset(&handler, 0, sizeof handler);
    handler.sa_handler = handlesig;
    sigemptyset(&handler.sa_mask);
    sigaddset(&handler.sa_mask, SIGTERM);
    sigaddset(&handler.sa_mask, SIGINT);
    sigset_t mask;
    int rc = -1;

    if (sigprocmask(SIG_BLOCK, &handler.sa_mask, &mask) < 0)
    {
	logmsg(L_ERROR, "cannot set signal mask");
	return -1;
    }

    if (sigaction(SIGTERM, &handler, 0) < 0)
    {
	logmsg(L_ERROR, "cannot set signal handler for SIGTERM");
	goto done;
    }

    if (sigaction(SIGINT, &handler, 0) < 0)
    {
	logmsg(L_ERROR, "cannot set signal handler for SIGINT");
	goto done;
    }

    logmsg(L_INFO, "service starting");
    for (;;)
    {
	fd_set rfds;
	fd_set wfds;
	fd_set *r = 0;
	fd_set *w = 0;
	if (nread)
	{
	    memcpy(&rfds, &readfds, sizeof rfds);
	    r = &rfds;
	}
	if (nwrite)
	{
	    memcpy(&wfds, &writefds, sizeof wfds);
	    w = &wfds;
	}
	int src = pselect(nfds, r, w, 0, 0, &mask);
	if (shutdownRequest)
	{
	    Event_raise(shutdown, 0, 0);
	    rc = 0;
	    logmsg(L_INFO, "service shutting down");
	    break;
	}
	if (src < 0)
	{
	    logmsg(L_ERROR, "pselect() failed");
	    break;
	}
	for (int i = 0; src > 0 && i < nfds; ++i)
	{
	    if (w && FD_ISSET(i, w))
	    {
		--src;
		Event_raise(readyWrite, i, 0);
	    }
	    if (r && FD_ISSET(i, r))
	    {
		--src;
		Event_raise(readyRead, i, 0);
	    }
	}
    }

done:
    if (sigprocmask(SIG_SETMASK, &mask, 0) < 0)
    {
	logmsg(L_ERROR, "cannot restore original signal mask");
	return -1;
    }

    return rc;
}

void Service_done(void)
{
    if (!cfg) return;
    Event_destroy(shutdown);
    Event_destroy(readyWrite);
    Event_destroy(readyRead);
    cfg = 0;
    shutdown = 0;
    readyWrite = 0;
    readyRead = 0;
}

