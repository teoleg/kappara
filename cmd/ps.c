#include "ulib.h"
void _start(void)
{
    char buf[1024];
    int fd = sys_open("/proc/ps", 0);
    if (fd < 0) { err("ps: cannot open /proc/ps\r\n"); sys_exit(); }
    long n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
        sys_write(1, buf, (size_t)n);
    sys_close(fd);
    sys_exit();
}
