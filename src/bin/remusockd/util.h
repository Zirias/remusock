#ifndef REMUSOCKD_UTIL_H
#define REMUSOCKD_UTIL_H

#include <stddef.h>
#include <stdio.h>

void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
char *copystr(const char *src);
char *joinstr(const char *delim, char **strings);

#endif
