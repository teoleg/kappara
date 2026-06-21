#ifndef _TIME_H
#define _TIME_H

#include <sys/types.h>

typedef long time_t;

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

int clock_gettime(int clk_id, struct timespec *ts);

/* time(NULL) returns CLOCK_MONOTONIC seconds. */
time_t time(time_t *t);

#endif /* _TIME_H */
