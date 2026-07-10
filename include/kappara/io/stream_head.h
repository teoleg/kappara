/*
 * include/kappara/io/stream_head.h -- STREAMS stream-head + fd plumbing
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

#include "kappara/proc/sched.h"	/* struct wait_queue */
#include "kappara/io/streams.h"

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
	unsigned	 sd_rd_tmo_ms;	/* I_SRDTMO read timeout; 0 = block
					 * forever (default).  Kappara-local:
					 * SVR4 would use poll(2), which we
					 * don't have.  See docs/DLPI.md. */
	struct stdata	*sd_peer;	/* pipe peer; non-NULL only for
					 * the two ends of a sys_pipe.    */
	struct wait_queue sd_readwait;	/* readers blocked on empty sd_rq */
	unsigned	 sd_last_reader;/* tid of most recent reader, used by
					 * the console TTY line-discipline so
					 * Ctrl-C still finds a target when no
					 * thread is blocked at the exact
					 * moment 0x03 arrives -- the typical
					 * race when a fast reader is draining
					 * single bytes one syscall at a time */
	struct stdata	*sd_all_next;	/* global list link -- every live
					 * stdata is on it so /proc/streams
					 * can walk all open instances    */
	/* Minor number of the underlying cdev, captured at open from
	 * MINOR(f->f_inode->i_rdev).  Zero for pipe ends and other
	 * non-cdev streams.  Multi-minor drivers (the tty driver from
	 * phase 3) read this from drv_rq->q_ptr in their qi_qopen to
	 * route to the right per-minor state. */
	unsigned	 sd_minor;
	unsigned	 sd_major;	/* resolved driver major; 0 for
					 * kernel-built streams + pipes */
	int		 sd_cloned;	/* minor came from cdev_minor_alloc;
					 * freed back on last close */
	/* SVR4 strioctl synchronisation.  stream_ioctl wraps a non-
	 * head ioctl as an M_IOCTL mblk, putnexts it down, sleeps on
	 * sd_ioc_wq, and waits for sh_rq_putp to catch an M_IOCACK or
	 * M_IOCNAK with a matching ic_tid.  sd_ioc_response is stashed
	 * by the head before the wake so the caller can read it off
	 * the wq side without re-walking the head's read queue. */
	struct wait_queue sd_ioc_wq;
	struct msgb      *sd_ioc_response;	/* mblk_t alias */

	/* Optional drain notification.  Set by an upstream module's
	 * qi_qopen (today: tcp).  stream_read / stream_getmsg invokes
	 * it after consuming bytes off sd_rq, so the module can react
	 * to "user has drained, flow control may have opened" without
	 * the head having to know what kind of module is upstream.
	 *
	 * Canonical use: TCP's window-update ACK.  When the receive
	 * window has shrunk to ~0 because the user-side reader fell
	 * behind, the peer sees window=0 and stops sending.  Once
	 * the user reads enough to materially re-open the window,
	 * TCP needs to issue a pure ACK so the peer learns there's
	 * room again.  Without this callback, the peer waits forever
	 * for an unsolicited window update -- which is exactly the
	 * cliff FTP STOR hit around 5 KB.
	 */
	void (*sd_on_drain)(struct stdata *sd, void *arg);
	void  *sd_on_drain_arg;
};

void streams_head_init(void);

/* In-kernel STREAMS API used by drivers and selftests that want to
 * build streams without going through sys_open / cdev paths. */
struct stdata *stream_build_kernel(struct streamtab *drv_st,
                                    const char *name, unsigned minor);
void           stream_destroy_kernel(struct stdata *sd);

/* In-kernel I_PUSH equivalent.  Used by the icmp boot selftest to
 * push the icmp module on top of an IP-driver stream without going
 * through sys_ioctl. */
int            stream_push_kernel  (struct stdata *sd,
                                    const char *modname);

/* I_LINK / I_UNLINK at the in-kernel API level.  The ioctl path
 * resolves fds to stdata and calls these.  Selftests can call them
 * directly.  Returns muxid > 0 on success, -1 on failure. */
long           stream_ilink  (struct stdata *upper, struct stdata *lower);
long           stream_iunlink(struct stdata *upper, int muxid);

/* Boot self-test exercising I_LINK end-to-end. */
void           mux_selftest(void);

/* Kthread entry point: poll PL011 RX FIFO and putnext received bytes
 * into the bottom of the active /dev/console read chain.  Spawned by
 * each arch's kmain before ksh so RX bytes don't get lost. */
void uart_rx_main(void *arg);

/* Register a driver or module under a name so sys_open / I_PUSH find it. */
void streams_register(const char *name, struct streamtab *st);

/* Look up a registered streamtab by name. */
struct streamtab *streams_lookup(const char *name);

/* Walk every registered module/driver in registration order; calls
 * cb(name, st, arg) for each.  Used by /proc/streams. */
void              streams_for_each(void (*cb)(const char *name,
					      struct streamtab *st,
					      void *arg),
				   void *arg);

/* Walk every open stream (every live stdata).  Used by /proc/streams
 * to list active opens with their queue stacks. */
void              streams_for_each_open(void (*cb)(struct stdata *sd,
						   void *arg),
					void *arg);

/* sys_open / sys_close / sys_read / sys_write / sys_ioctl / sys_putmsg /
 * sys_getmsg now live in include/kappara/fs/vfs.h -- they dispatch via the
 * VFS through each inode's file_ops vtable.  stream_fops is the
 * file_ops the VFS picks up when an opened inode is a character
 * special file whose i_private is a struct streamtab *. */
extern struct file_ops stream_fops;

/* ioctl commands.  SVR4 uses 'S' << 8 | n; we use plain ints for now. */
#define I_PUSH		1	/* arg = const char *modname            */
#define I_POP		2	/* arg = 0                              */
#define I_LIST		3	/* arg = char* buffer; fills with names */
#define I_LINK		4	/* arg = lower fd; returns muxid > 0    */
#define I_UNLINK	5	/* arg = muxid                          */
#define I_SRDTMO	6	/* arg = read timeout in ms; 0 = block
				 * (kappara-local -- stands in for poll) */

/*
 * SVR4 linkblk -- payload of the M_IOCTL{I_LINK} message sent down
 * the upper stream's write side.  The upper driver (mux) reads it
 * out of the b_cont mblk and records l_qbot in its per-instance
 * state so it can forward write-side traffic into the lower stream
 * via putnext(l_qbot, mp).  The stream head also rewires the lower
 * stream's read-side chain so messages going UP from the lower
 * driver enter the upper driver's read queue instead of the lower
 * stream head.
 *
 *   l_qtop   upper stream's driver write queue (advisory; the
 *            driver knows where it lives).
 *   l_qbot   lower stream's stream-head write queue.  putnext on
 *            this hands data into the lower stack at its top.
 *   l_index  muxid returned to the user; pass it to I_UNLINK.
 */
struct linkblk {
	queue_t	*l_qtop;
	queue_t	*l_qbot;
	int	 l_index;
};

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
