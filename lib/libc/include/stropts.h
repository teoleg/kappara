/*
 * <stropts.h> -- SVR4 STREAMS user-facing API.
 *
 * `strbuf` is the (maxlen, len, buf) triple used by putmsg / getmsg
 * to deliver M_PROTO control and M_DATA payload to / from a STREAMS
 * file descriptor.  putmsg packages ctl as M_PROTO and data as
 * M_DATA chained via b_cont; getmsg dequeues an mblk and splits it
 * back.  See uts/os/io/stream_head.c (stream_putmsg / stream_getmsg).
 */

#ifndef LIBC_STROPTS_H
#define LIBC_STROPTS_H

struct strbuf {
	int   maxlen;	/* on getmsg: capacity of buf  */
	int   len;	/* on putmsg: bytes to send; on getmsg: filled in */
	void *buf;
};

#define RS_HIPRI	0x01	/* high-priority message flag */

/* STREAMS ioctl commands -- must match include/kappara/stream_head.h. */
#define I_PUSH		1	/* arg = const char *modname */
#define I_POP		2	/* arg = 0                   */
#define I_LIST		3	/* arg = char* buffer        */
#define I_LINK		4	/* arg = lower stream fd     */
#define I_UNLINK	5	/* arg = muxid               */

int putmsg(int fd, const struct strbuf *ctl,
           const struct strbuf *data, int flags);
int getmsg(int fd, struct strbuf *ctl,
           struct strbuf *data, int *flagsp);
int ioctl (int fd, int cmd, long arg);

#endif
