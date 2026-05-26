/*
 * include/kappara/vfs.h -- minimal in-memory VFS + fd table
 * =========================================================
 *
 * What this is
 * ------------
 * The thinnest plausible Unix-style filesystem layer: an in-memory
 * tree of dentries and inodes, a file_ops vtable that file syscalls
 * dispatch through, and a fd table that holds open struct file
 * pointers.  Just enough to make sys_open("/dev/loop", 0) walk a
 * real path and find a character-special inode that wraps a
 * STREAMS driver, instead of looking drivers up by bare name.
 *
 * Today the only filesystem mounted at "/" is the in-memory rootfs
 * built at boot.  Eventually there will be ext-something, devfs,
 * procfs, etc. -- each providing its own dentry/inode/file_ops
 * implementation under this same shape.
 *
 *                +-----------+
 *                |   root /  |     dentry / inode (type=DIR)
 *                +-----+-----+
 *                      |
 *                +-----+-----+
 *                |   /dev    |     dentry / inode (type=DIR)
 *                +--+--+--+--+
 *                   |  |  |
 *                  loop console  ... character special files
 *                   |
 *           inode.i_type    = INODE_CHRDEV
 *           inode.i_fops    = &stream_fops
 *           inode.i_private = &loop_streamtab
 *
 * open("/dev/loop", 0) flow
 * -------------------------
 *   1. vfs_lookup("/dev/loop") walks the tree, returns the dentry
 *   2. allocate a struct file, set f_inode + f_ops from the inode
 *   3. call f_ops->open(file) -- for stream_fops this builds the
 *      stream head + driver stack, stashes stdata* in f_private
 *   4. fd_alloc returns the small int index
 *
 * read/write/ioctl/... just call file->f_ops->{read,write,...}.
 *
 * struct file
 * -----------
 *   f_ops      vtable picked up from the inode at open time
 *   f_inode    the inode opened
 *   f_private  per-open state (stream chrdev: struct stdata *)
 *   f_refs    refcount (dup-style sharing later)
 */

#ifndef KAPPARA_VFS_H
#define KAPPARA_VFS_H

#include <stddef.h>

#include "kappara/stream_head.h"	/* struct strbuf */

enum inode_type {
	INODE_DIR    = 1,
	INODE_CHRDEV = 2,
	/* INODE_REG, INODE_BLOCKDEV, INODE_SYMLINK ... later */
};

struct file;
struct inode;

struct file_ops {
	int   (*open)  (struct file *f);
	int   (*close) (struct file *f);
	long  (*read)  (struct file *f, void *buf, size_t len);
	long  (*write) (struct file *f, const void *buf, size_t len);
	long  (*ioctl) (struct file *f, int cmd, long arg);
	long  (*putmsg)(struct file *f, const struct strbuf *c,
			const struct strbuf *d, int flags);
	long  (*getmsg)(struct file *f, struct strbuf *c,
			struct strbuf *d, int *flagsp);
};

struct inode {
	enum inode_type   i_type;
	struct file_ops  *i_fops;
	void             *i_private;	/* chrdev: streamtab *  */
};

struct dentry {
	const char    *d_name;
	struct dentry *d_parent;
	struct dentry *d_sibling;	/* next child of d_parent */
	struct dentry *d_child;		/* first child (if dir)   */
	struct inode  *d_inode;
};

struct file {
	struct file_ops *f_ops;
	struct inode    *f_inode;
	void            *f_private;
	int              f_refs;
};

/* ---- Tree management ------------------------------------------------- */

void           vfs_init(void);
struct dentry *vfs_root(void);
struct dentry *vfs_lookup(const char *path);
struct dentry *vfs_mkdir(struct dentry *parent, const char *name);
struct dentry *vfs_mknod_chrdev(struct dentry *parent, const char *name,
				struct file_ops *fops, void *priv);

/* Pretty-print the tree rooted at d (use vfs_root() for the whole fs).
 * Walks the dentry tree recursively; chrdev nodes get tagged with
 * their inode type so you can tell directories from device files. */
void           vfs_dump_tree(struct dentry *d);

/* Fill `out` with NUL-terminated names of children of dir, separated
 * by '\n'.  Returns bytes written (without trailing NUL) or -1.
 * Truncates if `out` is too small.  Used by SYS_ls. */
long           vfs_listdir(struct dentry *dir, char *out, size_t cap);

/* ---- fd table ------------------------------------------------------- */

int           fd_alloc(struct file *f);
void          fd_free(int fd);
struct file  *fd_get(int fd);

/* ---- Syscall implementations (dispatch through f_ops) --------------- */

int   sys_open_impl(const char *path);
int   sys_close_impl(int fd);
long  sys_read_impl(int fd, void *buf, size_t len);
long  sys_write_impl(int fd, const void *buf, size_t len);
long  sys_ioctl_impl(int fd, int cmd, long arg);
long  sys_putmsg_impl(int fd, const struct strbuf *c,
		      const struct strbuf *d, int flags);
long  sys_getmsg_impl(int fd, struct strbuf *c,
		      struct strbuf *d, int *flagsp);

/* Anonymous STREAMS pipe.  Returns 0 and writes two fresh fds into
 * fds[2] (read end + write end -- both ends are actually bidirectional;
 * the names follow Unix convention).  Each end is a separate stdata
 * whose head_wq.q_next points at the peer's head_rq, so a write on
 * one side enqueues an mblk on the peer's read deferred list. */
long  sys_pipe_impl(int fds[2]);

#endif
