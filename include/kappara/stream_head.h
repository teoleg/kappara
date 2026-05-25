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

#include "kappara/streams.h"

struct stdata {
	queue_t		*sd_rq;	/* read queue at top of stack */
	queue_t		*sd_wq;	/* write queue at top of stack */
	int		 sd_refs;
	const char	*sd_name;
};

void streams_head_init(void);

/* Register a driver or module under a name so sys_open / I_PUSH find it. */
void streams_register(const char *name, struct streamtab *st);

/* Look up a registered streamtab by name. */
struct streamtab *streams_lookup(const char *name);

/* Open a registered driver by name, returning a fresh fd or -1. */
int  sys_open_impl(const char *name);
int  sys_close_impl(int fd);
long sys_read_impl(int fd, void *buf, size_t len);
long sys_write_impl(int fd, const void *buf, size_t len);
long sys_ioctl_impl(int fd, int cmd, long arg);

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

long sys_putmsg_impl(int fd, const struct strbuf *ctl,
		     const struct strbuf *data, int flags);
long sys_getmsg_impl(int fd, struct strbuf *ctl,
		     struct strbuf *data, int *flagsp);

#endif
