#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../aarch64/internal.h"

/* O_RDONLY / O_TRUNC flags matching kappara's syscall.h */
#define _O_RDONLY 0
#define _O_TRUNC  0x01

/* Static descriptors for stdin/stdout/stderr */
static FILE _stdin_f  = { .fd = 0 };
static FILE _stdout_f = { .fd = 1 };
static FILE _stderr_f = { .fd = 2 };

FILE *stdin  = &_stdin_f;
FILE *stdout = &_stdout_f;
FILE *stderr = &_stderr_f;

FILE *fopen(const char *path, const char *mode) {
    int flags = _O_RDONLY;
    if (mode && (mode[0] == 'w' || mode[0] == 'W')) flags = _O_TRUNC;
    long fd = __syscall3(__NR_open, (long)(unsigned long)path, (long)flags, 0);
    if (fd < 0) return NULL;
    FILE *f = malloc(sizeof(FILE));
    if (!f) { __syscall1(__NR_close, fd); return NULL; }
    f->fd   = (int)fd;
    f->_eof = 0;
    f->_err = 0;
    return f;
}

int fclose(FILE *f) {
    if (!f || f == stdin || f == stdout || f == stderr) return EOF;
    int r = (int)__syscall1(__NR_close, (long)f->fd);
    free(f);
    return r;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || size == 0 || nmemb == 0) return 0;
    ssize_t n = read(f->fd, ptr, size * nmemb);
    if (n < 0) { f->_err = 1; return 0; }
    if (n == 0) f->_eof = 1;
    return (size_t)n / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || size == 0 || nmemb == 0) return 0;
    ssize_t n = write(f->fd, ptr, size * nmemb);
    if (n < 0) { f->_err = 1; return 0; }
    return (size_t)n / size;
}

int fputc(int c, FILE *f) {
    unsigned char b = (unsigned char)c;
    if (write(f->fd, &b, 1) != 1) return EOF;
    return (int)b;
}

int fgetc(FILE *f) {
    unsigned char b;
    ssize_t n = read(f->fd, &b, 1);
    if (n <= 0) { f->_eof = (n == 0); f->_err = (n < 0); return EOF; }
    return (int)b;
}

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    if (write(f->fd, s, n) != (ssize_t)n) return EOF;
    return 1;
}

char *fgets(char *buf, int n, FILE *f) {
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    buf[i] = '\0';
    return buf;
}

int fseek(FILE *f, long offset, int whence) {
    long r = __syscall3(13 /* SYS_seek */, (long)f->fd, offset, (long)whence);
    return (r < 0) ? -1 : 0;
}

long ftell(FILE *f) {
    return __syscall3(13 /* SYS_seek */, (long)f->fd, 0L, 1 /* SEEK_CUR */);
}

int feof  (FILE *f) { return f->_eof; }
int ferror(FILE *f) { return f->_err; }
void clearerr(FILE *f) { f->_eof = 0; f->_err = 0; }

/* fprintf uses vsnprintf then writes to the file's fd */
int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) {
        size_t out = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
        write(f->fd, buf, out);
    }
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int putchar(int c) { return fputc(c, stdout); }

int puts(const char *s) {
    fputs(s, stdout);
    fputc('\n', stdout);
    return 1;
}
