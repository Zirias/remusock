#ifndef REMUSOCKD_LOG_H
#define REMUSOCKD_LOG_H

#include <stdio.h>

typedef enum LogLevel
{
    L_FATAL,    /**< program execution can't continue */
    L_ERROR,    /**< an error message, can't successfully complete */
    L_WARNING,  /**< a warning message, something seems wrong */
    L_INFO,     /**< an information message */
    L_DEBUG     /**< a debugging message, very verbose */
} LogLevel;

typedef void (*logwriter)(LogLevel level, const char *message, void *data);

void setFileLogger(FILE *file);
void setCustomLogger(logwriter writer, void *data);
void setMaxLogLevel(LogLevel level);

void logmsg(LogLevel level, const char *message);
void logfmt(LogLevel level, const char *format, ...);
void logsetsilent(int silent);
void writeFile(LogLevel level, const char *message, void *data);

#endif

