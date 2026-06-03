#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[1024];
    int fd = open("/proc/ps", 0);
    if (fd < 0) { puts("ps: cannot open /proc/ps"); return 1; }
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (size_t)n);
    close(fd);
    return 0;
}
