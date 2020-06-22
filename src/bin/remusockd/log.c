#include "log.h"
#include "threadpool.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

static logwriter currentwriter = 0;
static void *writerdata;
static LogLevel maxlevel = L_INFO;
static int logsilent = 0;

static const char *levels[] =
{
    "[FATAL]  ",
    "[ERROR]  ",
    "[WARN ]  ",
    "[INFO ]  ",
    "[DEBUG]  "
};

typedef struct LogJobArgs
{
    LogLevel level;
    logwriter writer;
    void *writerdata;
    char message[];
} LogJobArgs;

static void logmsgJobProc(void *arg)
{
    LogJobArgs *lja = arg;
    lja->writer(lja->level, lja->message, lja->writerdata);
    free(lja);
}

void writeFile(LogLevel level, const char *message, void *data)
{
    FILE *target = data;
    fputs(levels[level], target);
    fputs(message, target);
    fputc('\n', target);
    fflush(target);
}

void logmsg(LogLevel level, const char *message)
{
    if (!currentwriter) return;
    if (logsilent && level > L_ERROR) return;
    if (level > maxlevel) return;
    if (ThreadPool_active())
    {
	size_t msgsize = strlen(message)+1;
	LogJobArgs *lja = xmalloc(sizeof *lja + msgsize);
	lja->level = level;
	lja->writer = currentwriter;
	lja->writerdata = writerdata;
	strcpy(lja->message, message);
	ThreadJob *job = ThreadJob_create(logmsgJobProc, lja, 2);
	ThreadPool_enqueue(job);
    }
    else currentwriter(level, message, writerdata);
}

void logfmt(LogLevel level, const char *format, ...)
{
    if (!currentwriter) return;
    if (logsilent && level > L_ERROR) return;
    if (level > maxlevel) return;
    char buf[8192];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, 8192, format, ap);
    va_end(ap);
    logmsg(level, buf);
}

void logsetsilent(int silent)
{
    logsilent = silent;
}

void setFileLogger(FILE *file)
{
    currentwriter = writeFile;
    writerdata = file;
}

void setCustomLogger(logwriter writer, void *data)
{
    currentwriter = writer;
    writerdata = data;
}

void setMaxLogLevel(LogLevel level)
{
    maxlevel = level;
}

