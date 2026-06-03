#include <stdio.h>
#include <string.h>
#include "../user/syscall.h"

static void pipe_writer(long packed)
{
    int keep = (int)(packed >> 16), discard = (int)(packed & 0xffff);
    sys_close(discard);
    const char *msg = "pipe-writer: greetings from a second EL0 thread";
    sys_write(keep, msg, (size_t)strlen(msg));
    sys_close(keep);
    sys_exit();
}

static void pipe_reader(long packed)
{
    int keep = (int)(packed >> 16), discard = (int)(packed & 0xffff);
    sys_close(discard);
    for (;;) {
        char buf[80];
        long n = sys_read(keep, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        char log[120]; char *q = log;
        const char *p = "pipe-reader: got '"; while (*p) *q++ = *p++;
        for (long i = 0; i < n && q < log + sizeof(log) - 3; i++) *q++ = buf[i];
        *q++ = '\''; *q = '\0';
        sys_log(log);
    }
    sys_close(keep);
    sys_exit();
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int fds[2];
    if (sys_pipe(fds) < 0) { puts("pipework: sys_pipe failed"); return 1; }
    long wpacked = ((long)fds[1] << 16) | (long)(fds[0] & 0xffff);
    long rpacked = ((long)fds[0] << 16) | (long)(fds[1] & 0xffff);
    long wtid = sys_spawn(pipe_writer, wpacked);
    long rtid = sys_spawn(pipe_reader, rpacked);
    sys_close(fds[0]); sys_close(fds[1]);
    printf("pipework: writer tid=%ld, reader tid=%ld\n", wtid, rtid);
    if (wtid >= 0) sys_wait((int)wtid);
    if (rtid >= 0) sys_wait((int)rtid);
    return 0;
}
