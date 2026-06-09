#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
	int ok = 1;

	/* Basic alloc + write + verify */
	char *p = malloc(64);
	if (!p) {
		printf("malloctest: FAIL malloc(64) returned NULL\n");
		return 1;
	}
	for (int i = 0; i < 64; i++) p[i] = (char)i;
	for (int i = 0; i < 64; i++) {
		if (p[i] != (char)i) { ok = 0; break; }
	}
	if (!ok) {
		printf("malloctest: FAIL data mismatch in first alloc\n");
		return 1;
	}

	/* Second allocation must not overlap first */
	char *q = malloc(128);
	if (!q) {
		printf("malloctest: FAIL malloc(128) returned NULL\n");
		return 1;
	}
	if ((unsigned long)q < (unsigned long)p + 64) {
		printf("malloctest: FAIL second alloc overlaps first\n");
		return 1;
	}
	for (int i = 0; i < 128; i++) q[i] = (char)(i ^ 0x55);
	for (int i = 0; i < 128; i++) {
		if (q[i] != (char)(i ^ 0x55)) { ok = 0; break; }
	}
	if (!ok) {
		printf("malloctest: FAIL data mismatch in second alloc\n");
		return 1;
	}

	/* free + realloc */
	free(p);
	char *r = malloc(32);
	if (!r) {
		printf("malloctest: FAIL malloc(32) after free returned NULL\n");
		return 1;
	}
	memset(r, 0xAB, 32);
	for (int i = 0; i < 32; i++) {
		if ((unsigned char)r[i] != 0xAB) { ok = 0; break; }
	}
	if (!ok) {
		printf("malloctest: FAIL data mismatch after free+realloc\n");
		return 1;
	}

	/* calloc: must return zeroed memory */
	int *arr = calloc(16, sizeof(int));
	if (!arr) {
		printf("malloctest: FAIL calloc returned NULL\n");
		return 1;
	}
	for (int i = 0; i < 16; i++) {
		if (arr[i] != 0) { ok = 0; break; }
	}
	if (!ok) {
		printf("malloctest: FAIL calloc memory not zeroed\n");
		return 1;
	}

	/* First write to heap (q) survived the calloc alloc? */
	for (int i = 0; i < 128; i++) {
		if (q[i] != (char)(i ^ 0x55)) { ok = 0; break; }
	}
	if (!ok) {
		printf("malloctest: FAIL q corrupted by subsequent allocs\n");
		return 1;
	}

	printf("malloctest: PASS\n");
	return 0;
}
