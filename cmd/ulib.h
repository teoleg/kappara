#ifndef CMD_ULIB_H
#define CMD_ULIB_H

#include "../user/syscall.h"

static inline size_t u_len(const char *s) { size_t n=0; while(s[n]) n++; return n; }

static inline void out(const char *s) { sys_write(1, s, u_len(s)); }
static inline void err(const char *s) { sys_write(2, s, u_len(s)); }

static inline void out_long(long v)
{
    char tmp[24]; int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { long x = v < 0 ? -v : v; while (x) { tmp[n++] = (char)('0' + x % 10); x /= 10; } if (v < 0) tmp[n++] = '-'; }
    char buf[24]; for (int i = 0; i < n; i++) buf[i] = tmp[n-1-i]; buf[n] = 0;
    sys_write(1, buf, (size_t)n);
}

static inline void out_hex(unsigned long v)
{
    char tmp[20]; int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = "0123456789abcdef"[v & 0xf]; v >>= 4; } }
    char buf[20]; for (int i = 0; i < n; i++) buf[i] = tmp[n-1-i]; buf[n] = 0;
    sys_write(1, buf, (size_t)n);
}

#endif
