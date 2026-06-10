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

int putmsg(int fd, const struct strbuf *ctl,
           const struct strbuf *data, int flags);
int getmsg(int fd, struct strbuf *ctl,
           struct strbuf *data, int *flagsp);

#endif
