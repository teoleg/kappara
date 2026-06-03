#include "ulib.h"
void _start(void) {
    int fds[2];
    if (sys_pipe(fds) < 0) { err("pipe: failed\r\n"); sys_exit(); }
    out("pipe: rd_fd="); out_long(fds[0]); out(" wr_fd="); out_long(fds[1]); out("\r\n");
    const char *msg = "hello through the streams pipe!";
    long w = sys_write(fds[1], msg, u_len(msg));
    out("pipe: wrote "); out_long(w); out(" bytes\r\n");
    char buf[64];
    long n = sys_read(fds[0], buf, sizeof(buf));
    out("pipe: read "); out_long(n); out(" -> '");
    sys_write(1, buf, (size_t)(n > 0 ? n : 0));
    out("'\r\n");
    sys_close(fds[0]); sys_close(fds[1]);
    sys_exit();
}
