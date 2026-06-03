#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
void  exit (int status) __attribute__((noreturn));
void _exit (int status) __attribute__((noreturn));
int   atoi (const char *s);
long  atol (const char *s);
long  strtol(const char *s, char **endp, int base);
void *malloc (size_t size);
void *calloc (size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free   (void *ptr);
#endif /* _STDLIB_H */
