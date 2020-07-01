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
    char *copy = xmalloc(strlen(src) + 1);
    strcpy(copy, src);
    return copy;
}

char *joinstr(const char *delim, char **strings)
{
    int n = 0;
    size_t rlen = 0;
    size_t dlen = strlen(delim);
    char **cur;
    for (cur = strings; *cur; ++cur)
    {
	++n;
	rlen += strlen(*cur);
    }
    if (!n) return 0;
    if (n > 1)
    {
	rlen += (n - 1) * dlen;
    }
    char *joined = xmalloc(rlen + 1);
    strcpy(joined, *strings);
    char *w = joined + strlen(*strings);
    cur = strings+1;
    while (*cur)
    {
	strcpy(w, delim);
	w += dlen;
	strcpy(w, *cur);
	w += strlen(*cur);
	++cur;
    }
    return joined;
}

