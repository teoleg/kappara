#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

/* FILE -- thin unbuffered layer; buffering added later */
typedef struct _FILE {
    int fd;
    int _eof;
    int _err;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* printf family (unchanged -- write to fd 1 directly) */
int printf (const char *fmt, ...) __attribute__((format(printf,1,2)));
int fprintf (FILE *f, const char *fmt, ...) __attribute__((format(printf,2,3)));
int sprintf (char *buf,           const char *fmt, ...) __attribute__((format(printf,2,3)));
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf,3,4)));
int vprintf (const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* character / line I/O */
int  puts  (const char *s);
int  fputs (const char *s, FILE *f);
int  putchar(int c);
int  getchar(void);
int  fputc (int c, FILE *f);
int  fgetc (FILE *f);
char *fgets(char *buf, int n, FILE *f);

/* getline -- read until '\n' or EOF into a malloc'd buffer.  *bufp /
 * *cap are reused/grown across calls.  Returns the byte count read
 * (including the trailing '\n' if present), or -1 on EOF/error. */
ssize_t getline(char **bufp, size_t *cap, FILE *f);

/* perror -- write "prefix: <human message>\n" to stderr.  Today the
 * kernel doesn't populate errno, so this prints "<prefix>: error". */
void perror(const char *prefix);

/* fflush is a no-op -- the FILE layer is unbuffered.  Provided so
 * code that calls fflush(stdout) compiles. */
int  fflush(FILE *f);

/* file ops */
FILE  *fopen (const char *path, const char *mode);
int    fclose(FILE *f);
size_t fread (void       *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int    fseek (FILE *f, long offset, int whence);
long   ftell (FILE *f);
int    feof  (FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int proc_cat(const char *path, int strip_preamble);

#define EOF (-1)

#endif
