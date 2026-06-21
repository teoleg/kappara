#include <time.h>
#include "../aarch64/internal.h"

int clock_gettime(int clk_id, struct timespec *ts)
{
    return (int)__syscall3(__NR_clock_gettime,
                           (long)clk_id, (long)(unsigned long)ts, 0);
}

time_t time(time_t *t)
{
    struct timespec ts = { 0, 0 };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (t)
        *t = ts.tv_sec;
    return ts.tv_sec;
}
