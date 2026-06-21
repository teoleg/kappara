/*
 * cmd/uptime.c -- print monotonic seconds since boot.
 *
 * Exercises Stage 1's <time.h> + clock_gettime syscall; doubles as
 * a real /usr/bin tool.  No flags; output shape:
 *
 *     up 42s
 *     up 1h 03m 12s
 *
 * Drops the day field once we have a non-monotonic clock.
 */

#include <stdio.h>
#include <time.h>

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
		printf("uptime: clock_gettime failed\n");
		return 1;
	}

	long secs = ts.tv_sec;
	long h = secs / 3600;
	long m = (secs % 3600) / 60;
	long s = secs % 60;

	if (h > 0)
		printf("up %ldh %02ldm %02lds\n", h, m, s);
	else if (m > 0)
		printf("up %ldm %02lds\n", m, s);
	else
		printf("up %lds\n", s);

	return 0;
}
