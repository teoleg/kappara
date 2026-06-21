#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
void  exit (int status) __attribute__((noreturn));
void _exit (int status) __attribute__((noreturn));
int   atoi (const char *s);
long  atol (const char *s);
long  strtol(const char *s, char **endp, int base);
int   abs   (int n);
long  labs  (long n);
char *getenv(const char *name);  /* stub: always NULL */
void  qsort (void *base, size_t nmemb, size_t size,
             int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *));
void *malloc (size_t size);
void *calloc (size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free   (void *ptr);
#endif /* _STDLIB_H */
