#ifndef _STDLIB_H
#define _STDLIB_H
void  exit (int status) __attribute__((noreturn));
void _exit (int status) __attribute__((noreturn));
int   atoi (const char *s);
long  atol (const char *s);
long  strtol(const char *s, char **endp, int base);
#endif /* _STDLIB_H */
