/*
 * include/kappara/streams.h -- SVR4-style STREAMS framework
 * =========================================================
 *
 * This is the first piece of the STREAMS subsystem.  The names and
 * data structures match Unix System V Release 4 conventions so the
 * code reads the same as a 1990s SVR4 driver: mblk_t / dblk_t /
 * queue_t / qinit / module_info, plus allocb / freeb / freemsg /
 * putq / getq / putnext.
 *
 * Why STREAMS at all
 * ------------------
 * STREAMS turns any character-ish I/O path (tty, pipe, network,
 * pseudo-devices) into a stack of small, composable modules with
 * message-passing between them.  A user does open() on a stream
 * head; data flows up and down as mblk_t messages through the
 * module stack to a driver at the bottom.  Modules can be pushed
 * and popped at runtime (I_PUSH / I_POP) so the same driver can
 * be wrapped in line discipline, packetisation, encryption, ...
 *
 *      user (read / write / putmsg / getmsg via the stream head)
 *         |
 *         v
 *      +-------------+    stream head -- the boundary with user code
 *      |  sthread    |
 *      +-------------+
 *         | ^
 *         v |
 *      +-------------+    a pushed module, e.g. ldterm (line discipline)
 *      | module N    |
 *      +-------------+
 *         | ^
 *         v |
 *      +-------------+
 *      | ...         |
 *      +-------------+
 *         | ^
 *         v |
 *      +-------------+    bottom of the stack: the driver
 *      | driver      |    (e.g. PL011 UART driver as a STREAMS driver)
 *      +-------------+
 *         | ^
 *         v |
 *       hardware
 *
 * Each module exposes a paired queue_t (read queue going up,
 * write queue going down).  Messages flow downstream via
 * putnext(WR(q), mp) and upstream via putnext(RD(q), mp).
 *
 * Messages: mblk_t + dblk_t
 * -------------------------
 *
 *   mblk_t     header / linkage (cheap)
 *   dblk_t     "data block": the buffer pointer + length + refcount
 *
 *      +----------+        +-----------+        +---------------+
 *      |  mblk #1 | -----> | dblk      | -----> | buffer bytes  |
 *      |          |        |  db_ref=2 |        |               |
 *      +----------+        +-----------+        +---------------+
 *           ^                   ^
 *           | b_cont            |
 *           v                   |
 *      +----------+             |
 *      |  mblk #2 |-------------+   (dupb sharing the same dblk)
 *      +----------+
 *
 * Two mblks can share one dblk -- that's how SVR4 does cheap
 * message duplication (dupb / dupmsg).  freeb on the last
 * referencing mblk frees the dblk and its buffer.
 *
 * b_next chains messages in a queue's deferred-message list.
 * b_cont chains continuation pieces of the SAME message (a long
 * read split across multiple buffers, say).
 *
 * Message types (db_type)
 * -----------------------
 *   M_DATA   ordinary data (the vast majority)
 *   M_PROTO  protocol-control information that travels with data
 *   M_CTL    module-private control
 *   M_FLUSH  flush queues
 *   M_BREAK  line break (tty)
 *   ...      many more in real SVR4; we'll add as needed
 */

#ifndef KAPPARA_STREAMS_H
#define KAPPARA_STREAMS_H

#include <stddef.h>
#include <stdint.h>

/* ---- Message types ------------------------------------------------------ */

#define M_DATA		0x00	/* ordinary data */
#define M_PROTO		0x01	/* protocol control information */
#define M_CTL		0x0d	/* module-private control */
#define M_BREAK		0x08	/* line break */
#define M_FLUSH		0x06	/* flush queues */
#define M_ERROR		0x05	/* error from driver / module */

/* ---- Data block --------------------------------------------------------- */

struct dblk {
	unsigned char	*db_base;	/* start of data buffer */
	unsigned char	*db_lim;	/* one past end of buffer */
	int		 db_ref;	/* how many mblks point at this dblk */
	unsigned char	 db_type;	/* M_DATA, M_PROTO, ... */
};

typedef struct dblk dblk_t;

/* ---- Message block ------------------------------------------------------ */

struct msgb {
	struct msgb	*b_next;	/* next message in this queue */
	struct msgb	*b_prev;	/* previous message in this queue */
	struct msgb	*b_cont;	/* continuation of THIS message */
	unsigned char	*b_rptr;	/* read pointer within db_base..db_lim */
	unsigned char	*b_wptr;	/* write pointer */
	struct dblk	*b_datap;	/* the data block we point at */
	unsigned char	 b_band;	/* priority band */
	unsigned char	 b_flag;	/* per-mblk flags */
};

typedef struct msgb mblk_t;

/* ---- Module info -------------------------------------------------------- */

struct module_info {
	unsigned short	 mi_idnum;	/* module ID */
	const char	*mi_idname;	/* module name */
	long		 mi_minpsz;	/* min packet size */
	long		 mi_maxpsz;	/* max packet size */
	long		 mi_hiwat;	/* high water mark */
	long		 mi_lowat;	/* low water mark */
};

/* ---- qinit -- per-direction module config ------------------------------- */

struct queue;

struct qinit {
	int		(*qi_putp)(struct queue *q, mblk_t *mp);
	int		(*qi_srvp)(struct queue *q);
	int		(*qi_qopen)(struct queue *q);
	int		(*qi_qclose)(struct queue *q);
	struct module_info *qi_minfo;
};

/* ---- queue -- one direction of a module pair ---------------------------- */

#define QFULL		(1u << 0)	/* hit hi water mark */
#define QWANTR		(1u << 1)	/* reader is waiting */
#define QWANTW		(1u << 2)	/* writer is waiting */
#define QENAB		(1u << 3)	/* on the runqueue, srvp pending */

struct queue {
	struct qinit	*q_qinfo;	/* put / srv / open / close */
	mblk_t		*q_first;	/* head of deferred message list */
	mblk_t		*q_last;	/* tail of deferred message list */
	struct queue	*q_next;	/* the queue downstream of this one */
	struct queue	*q_link;	/* OTHERQ pairing */
	struct queue	*q_runlink;	/* next queue on the service runqueue */
	void		*q_ptr;		/* module-private state */
	size_t		 q_count;	/* bytes currently queued */
	long		 q_minpsz;	/* per-queue copies of mi_* limits */
	long		 q_maxpsz;
	long		 q_hiwat;
	long		 q_lowat;
	unsigned	 q_flag;	/* QFULL / QWANT* / QENAB */
};

typedef struct queue queue_t;

/* ---- streamtab: a module/driver's pair of qinits ------------------------ */

struct streamtab {
	struct qinit *st_rdinit;	/* read-side  (upstream) qinit */
	struct qinit *st_wrinit;	/* write-side (downstream) qinit */
};

/* ---- Public API --------------------------------------------------------- */

void    streams_init(void);

mblk_t *allocb(size_t size, unsigned pri);
void    freeb(mblk_t *mp);
void    freemsg(mblk_t *mp);
size_t  msgdsize(const mblk_t *mp);

void    queue_init_pair(queue_t *rq, queue_t *wq,
			struct qinit *rinit, struct qinit *winit);

void    putq(queue_t *q, mblk_t *mp);
mblk_t *getq(queue_t *q);
int     putnext(queue_t *q, mblk_t *mp);

/*
 * Service-procedure machinery.
 *
 *   qenable(q)     schedule q's qi_srvp to be called by streams_run.
 *                  Idempotent: a queue already on the runqueue is not
 *                  re-added.
 *   streams_run()  drain the runqueue until empty.  Called from the
 *                  timer-tick path and from sys_yield, so any module
 *                  whose putp defers work via putq + qenable will see
 *                  that work picked up the next time we're idle.
 */
void    qenable(queue_t *q);
void    streams_run(void);

/* Travel-direction sugar.  In SVR4 these are macros that exploit the
 * standard layout where read and write queues come allocated as a
 * pair with WR(q) = q + 1, RD(q) = q - 1.  We allocate them
 * separately for now and just look up via q_link or a parallel
 * pointer kept in q_ptr; pending later cleanup. */
#define WR(q)	((q)->q_link)
#define RD(q)	((q)->q_link)
#define OTHERQ(q) ((q)->q_link)

#endif
