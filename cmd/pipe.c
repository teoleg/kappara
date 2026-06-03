#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../user/syscall.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int fds[2];
    if (sys_pipe(fds) < 0) { puts("pipe: failed"); return 1; }
    printf("pipe: rd_fd=%d wr_fd=%d\n", fds[0], fds[1]);
    const char *msg = "hello through the streams pipe!";
    long w = write(fds[1], msg, strlen(msg));
    printf("pipe: wrote %ld bytes\n", w);
    char buf[64];
    long n = read(fds[0], buf, sizeof(buf));
    printf("pipe: read %ld -> '", n);
    if (n > 0) write(1, buf, (size_t)n);
    puts("'");
    close(fds[0]); close(fds[1]);
    return 0;
}
