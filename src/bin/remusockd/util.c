#include <stdlib.h>
#include <string.h>

#include "log.h"

#include "util.h"

void *xmalloc(size_t size)
{
    void *m = malloc(size);
    if (!m)
    {
        logmsg(L_FATAL, "memory allocation failed.");
        abort();
    }
    return m;
}

void *xrealloc(void *ptr, size_t size)
{
    void *m = realloc(ptr, size);
    if (!m)
    {
        logmsg(L_FATAL, "memory allocation failed.");
        abort();
    }
    return m;
}

char *copystr(const char *src)
{
    if (!src) return 0;
    char *copy = xmalloc(strlen(src) +1);
    strcpy(copy, src);
    return copy;
}

