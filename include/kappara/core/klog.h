/*
 * include/kappara/klog.h -- kernel log ring buffer
 *
 * kprintf tees every byte it emits into klog_buf via klog_putc, so
 * the boot log lives in RAM after the bytes have scrolled off the
 * UART.  /dev/klog (a STREAMS character device) lets a reader
 * (typically ksh) snapshot the ring into a stream's read queue.
 *
 *   kprintf -- uart_putc -> PL011 (immediate)
 *           \- klog_putc -> klog_buf  (persistent for the boot)
 *
 *   sys_open("/dev/klog")  -> seeds the stream's rq with a snapshot
 *                              of klog_buf as a chain of mblks
 *   sys_read fd -> drains the snapshot a chunk at a time
 *
 * The ring is non-wrapping for now (drops new bytes when full); a
 * proper wrapping ring with a head/tail pair lives in real
 * implementations (Linux dmesg).
 */
#ifndef KAPPARA_KLOG_H
#define KAPPARA_KLOG_H

#include <stddef.h>

void   klog_putc(char c);

/* Bytes currently held in the ring (0..klog capacity). */
size_t klog_size(void);

/* Copy at most cap bytes starting at offset `off` into out.  Returns
 * the number actually copied.  Used by the /dev/klog driver to fill
 * mblks at open time. */
size_t klog_copy(char *out, size_t off, size_t cap);

#endif
