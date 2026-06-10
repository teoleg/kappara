/*
 * cmd/forktest -- exercise sys_fork
 *
 * Smoke test for R5:
 *   - parent forks
 *   - child sees its own copy of the address space (writes don't
 *     escape to the parent)
 *   - child exits with a known status; parent waits and verifies it
 *
 * Each side writes through its own copy of a shared variable; we
 * verify the parent's view is unchanged after the child runs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int shared = 0xAA;

int main(void)
{
	int before = shared;
	pid_t kid = fork();
	if (kid < 0) {
		printf("forktest: FAIL fork returned %d\n", (int)kid);
		return 1;
	}

	if (kid == 0) {
		/* Child.  Mutate `shared`; if the address space is really
		 * private, the parent's copy should still read 0xAA after
		 * we exit. */
		shared = 0xBB;
		if (shared != 0xBB) {
			_exit(11);
		}
		_exit(42);
	}

	/* Parent. */
	int rc = wait((int)kid);
	if (rc != 42) {
		printf("forktest: FAIL child exit status %d (want 42)\n", rc);
		return 1;
	}
	if (shared != before) {
		printf("forktest: FAIL parent shared=%x (want %x), "
		       "child mutation leaked\n", shared, before);
		return 1;
	}

	printf("forktest: PASS (child=%d shared=%x)\n", (int)kid, shared);
	return 0;
}
