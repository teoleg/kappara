/*
 * include/kappara/stream_head.h -- STREAMS stream-head + fd plumbing
 *
 * The stream head is the per-open piece that sits at the top of a
 * STREAMS stack and bridges user-facing syscalls (read / write /
 * ioctl) into the message-passing world below.
 *
 * Stream stack with no modules pushed:
 *
 *     +-----------+    sd_rq <-- user reads here (sys_read pops mblks)
 *     |  stdata   |
 *     |   head    |    sd_wq --> user writes here (sys_write puts here)
 *     +-----+-----+
 *           | wq going DOWN: head_wq -> drv_wq
 *           | rq going UP:   drv_rq -> head_rq
 *           v
 *     +-----------+
 *     |  driver   |    bottom of the stack
 *     +-----------+
 *
 * After I_PUSH "upper":
 *
 *     +-----------+
 *     |  stdata   |
 *     +-----+-----+
 *           |
 *           v wq                            ^  rq
 *     +-----------+                         |
 *     |  upper    |   (inserted just below head)
 *     +-----+-----+
 *           |                               |
 *           v                               |
 *     +-----------+                         |
 *     |  driver   |                         |
 *     +-----------+                         |
 *
 * I_POP removes the topmost module (never the driver).
 *
 * fd table (sys_open / sys_close / sys_read / sys_write / sys_ioctl)
 * -----------------------------------------------------------------
 * A small global fd_table[FD_MAX] of struct file points at the open
 * streams.  sys_open takes a registered driver name (e.g. "loop"),
 * builds a head + driver pair, returns an fd.  Until processes show
 * up, "global" is the right granularity.
 */
#ifndef KAPPARA_STREAM_HEAD_H
#define KAPPARA_STREAM_HEAD_H

#include <stddef.h>

#include "kappara/sched.h"	/* struct wait_queue */
#include "kappara/streams.h"

/* sd_flags bits. */
#define SD_EOF		(1u << 0)	/* peer (if any) has closed; reads
					 * past the queued backlog return 0 */

struct stdata {
	queue_t		*sd_rq;	/* read queue at top of stack */
	queue_t		*sd_wq;	/* write queue at top of stack */
	queue_t		*sd_drv_rq;	/* driver's read queue (NULL for
					 * pipes -- no driver at all)     */
	queue_t		*sd_drv_wq;	/* driver's write queue           */
	int		 sd_refs;
	const char	*sd_name;
	unsigned	 sd_flags;	/* SD_EOF / future SD_ERR etc.   */
	struct stdata	*sd_peer;	/* pipe peer; non-NULL only for
					 * the two ends of a sys_pipe.    */
	struct wait_queue sd_readwait;	/* readers blocked on empty sd_rq */
};

void streams_head_init(void);

/* Kthread entry point: poll PL011 RX FIFO and putnext received bytes
 * into the bottom of the active /dev/console read chain.  Spawned by
 * each arch's kmain before ksh so RX bytes don't get lost. */
void uart_rx_main(void *arg);

/* Register a driver or module under a name so sys_open / I_PUSH find it. */
void streams_register(const char *name, struct streamtab *st);

/* Look up a registered streamtab by name. */
struct streamtab *streams_lookup(const char *name);

/* sys_open / sys_close / sys_read / sys_write / sys_ioctl / sys_putmsg /
 * sys_getmsg now live in include/kappara/vfs.h -- they dispatch via the
 * VFS through each inode's file_ops vtable.  stream_fops is the
 * file_ops the VFS picks up when an opened inode is a character
 * special file whose i_private is a struct streamtab *. */
extern struct file_ops stream_fops;

/* ioctl commands.  SVR4 uses 'S' << 8 | n; we use plain ints for now. */
#define I_PUSH		1	/* arg = const char *modname            */
#define I_POP		2	/* arg = 0                              */
#define I_LIST		3	/* arg = char* buffer; fills with names */

/*
 * SVR4 strbuf -- (maxlen, len, buf) triple used by putmsg / getmsg.
 *   putmsg:  len = bytes to send, maxlen ignored
 *   getmsg:  maxlen = buffer capacity, len gets set to bytes returned
 */
struct strbuf {
	int   maxlen;
	int   len;
	void *buf;
};

#define RS_HIPRI	0x01	/* high-priority message flag */

#endif
