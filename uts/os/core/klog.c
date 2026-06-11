/*
 * kernel/klog.c -- kernel log ring buffer
 * =======================================
 *
 * A small RAM-resident buffer that holds every byte kprintf has
 * emitted since boot.  The mirror is built into printk.c: each
 * byte that goes out via uart_putc is also fed here via klog_putc.
 *
 *      kprintf("foo")
 *          |
 *          v   (kernel/printk.c)
 *     for each byte:
 *         uart_putc(b)     -- immediate, to PL011 TX
 *         klog_putc(b)     -- linger here for later replay
 *
 * Why have it
 * -----------
 * The console scrolls.  By the time a user reaches the shell, all
 * the interesting messages from mmu_init/pmm_init/etc are gone --
 * unless we kept a copy.  klog is that copy.
 *
 * Buffer policy
 * -------------
 * Non-wrapping, fixed-size, drop-new-bytes-when-full.  Small, simple,
 * and biased toward the boot story (which is the most useful log to
 * keep -- not whatever the most recent N bytes happen to be).  When
 * we want wrap-around we can switch to a (head, tail, full) trio.
 *
 * Storage
 * -------
 * KLOG_SIZE bytes in BSS.  At 8 KB this is bigger than the slab
 * allocator's biggest object (2 KB) but trivial vs. the GB of RAM
 * we have.  The /dev/klog driver chunks the snapshot into multiple
 * smaller mblks so allocb's size limit is respected.
 */

#include <stddef.h>

#include "kappara/core/klog.h"

#define KLOG_SIZE	8192

static char   klog_buf[KLOG_SIZE];
static size_t klog_len;

void klog_putc(char c)
{
	if (klog_len < KLOG_SIZE)
		klog_buf[klog_len++] = c;
}

size_t klog_size(void)
{
	return klog_len;
}

size_t klog_copy(char *out, size_t off, size_t cap)
{
	if (off >= klog_len)
		return 0;
	size_t avail = klog_len - off;
	size_t n     = (avail < cap) ? avail : cap;
	for (size_t i = 0; i < n; i++)
		out[i] = klog_buf[off + i];
	return n;
}
