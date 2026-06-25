/*
 * <sys/mman.h> -- kappara mmap interface
 *
 * Supported on kappara today:
 *   - MAP_ANONYMOUS: zero-fill, fd ignored
 *   - MAP_PRIVATE  + fd: eager file-backed mapping
 * Caller-provided `addr` is ignored (kernel always picks).  `prot`
 * is accepted but every mapping is currently user-RW; real PROT_NONE /
 * PROT_READ-only landing is a follow-up.
 *
 * Constants match Linux ABI so kappara binaries and Linux-ABI binaries
 * share the same numbers -- the kernel-side translation lives in
 * sys_mmap_impl, not in the wrappers.
 */
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>

#define PROT_NONE     0x00
#define PROT_READ     0x01
#define PROT_WRITE    0x02
#define PROT_EXEC     0x04

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED    ((void *)-1)

void *mmap (void *addr, size_t length, int prot, int flags,
            int fd, long offset);
int   munmap(void *addr, size_t length);

#endif /* _SYS_MMAN_H */
