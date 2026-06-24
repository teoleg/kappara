/*
 * include/kappara/fs/vfs.h -- minimal in-memory VFS + fd table
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
#include <stdint.h>

#include "kappara/io/stream_head.h"	/* struct strbuf */

enum inode_type {
	INODE_DIR    = 1,
	INODE_CHRDEV = 2,
	INODE_REG    = 3,	/* regular file backed by a filesystem driver */
	/* INODE_BLOCKDEV, INODE_SYMLINK ... later */
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
	long  (*seek)  (struct file *f, long offset, int whence);
	/* Directory-side ops: create a regular file or a subdirectory
	 * named `name` under the inode `dir`.  Only meaningful on
	 * directory inodes whose underlying filesystem supports it. */
	int   (*creat) (struct inode *dir, const char *name);
	int   (*mkdir) (struct inode *dir, const char *name);
	int   (*unlink)(struct inode *dir, const char *name);
	int   (*rmdir) (struct inode *dir, const char *name);
	/* Optional: return the file's current size in bytes for ls -l.
	 * Operates on the inode (not an open file) so callers don't have
	 * to open just to stat. */
	long  (*size)  (struct inode *ino);
	/* SVR4 vop_inactive: called by vfs_iput when the inode's last
	 * reference drops.  The FS releases whatever lived behind
	 * i_private (kfs_file, kfs_dir, ...).  After this returns the
	 * VFS frees the inode itself. */
	void  (*inactive)(struct inode *ino);
};

/* sys_open / file open flags. */
#define O_TRUNC		0x01	/* size -> 0 on open */

struct inode {
	enum inode_type   i_type;
	struct file_ops  *i_fops;
	void             *i_private;	/* dir/reg: FS-specific meta  */
	/* For CHRDEV inodes: the SVR4 device id.  MAJOR(i_rdev) indexes
	 * the cdevsw[]; MINOR distinguishes instances of the same
	 * driver (think /dev/tty0 vs /dev/tty1).  Unused for dirs and
	 * regular files. */
	uint32_t          i_rdev;
	/* Reference count -- vnode.v_count in SVR4 terms.  Starts at 1
	 * (the dentry holds the inode).  Every open bumps it; every
	 * close drops it.  When it falls to 0, vfs_iput calls
	 * i_fops->inactive (vop_inactive) to release the FS-private
	 * data and then frees the inode itself.  This is the lifecycle
	 * model SVR4 used; Linux's separate dcache/inode refcounting
	 * is a different shape -- we don't need it here.
	 *
	 * Touched ONLY via vfs_iget / vfs_iput, which route through
	 * atomic_inc / atomic_dec_and_test in include/kappara/core/atomic.h
	 * so concurrent close() on multiple CPUs can't race the drop.
	 * Plain `++` / `--` is a bug.  See the matching contract on
	 * struct file::f_refs below. */
	int               i_count;
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
	/* SVR4 f_count.  Number of fdt slots (across every thread)
	 * that still name this file.  Touched ONLY via file_get /
	 * file_put inside vfs.c -- those route through atomic_inc /
	 * atomic_dec_and_test (include/kappara/core/atomic.h) so
	 * concurrent dup() + close() on different CPUs can't lose an
	 * increment or both think they did the last drop.  Initial
	 * value is assigned plain at struct-creation time (no other
	 * CPU has the pointer yet). */
	int              f_refs;
	int              f_flags;	/* O_TRUNC | ...  set by sys_open */
};

/* ---- Tree management ------------------------------------------------- */

void           vfs_init(void);
struct dentry *vfs_root(void);
struct dentry *vfs_lookup(const char *path);
struct dentry *vfs_mkdir(struct dentry *parent, const char *name);
/* Create a character-device dentry.  `rdev` is the SVR4 dev_t --
 * MAJOR(rdev) must already be registered in cdevsw[] (see
 * cdevsw.h).  The VFS open path uses MAJOR(rdev) to find the
 * driver's streamtab; MINOR is passed to whatever driver-specific
 * open hook cares.  The inode's i_fops is set to a stock chrdev
 * vtable that funnels through STREAMS. */
struct dentry *vfs_mknod_chrdev(struct dentry *parent, const char *name,
				uint32_t rdev);

/* Same shape as mknod_chrdev but creates an INODE_REG -- used by
 * filesystem drivers (kfs today) to publish files they discovered
 * on disk.  priv typically points at the FS-specific metadata
 * (struct kfs_file *, etc.) the file_ops will dereference. */
struct dentry *vfs_mknod_regfile(struct dentry *parent, const char *name,
				 struct file_ops *fops, void *priv);

/* Pretty-print the tree rooted at d (use vfs_root() for the whole fs).
 * Walks the dentry tree recursively; chrdev nodes get tagged with
 * their inode type so you can tell directories from device files. */
void           vfs_dump_tree(struct dentry *d);

/* Fill `out` with NUL-terminated names of children of dir, separated
 * by '\n'.  Returns bytes written (without trailing NUL) or -1.
 * Truncates if `out` is too small.  Used by SYS_ls. */
long           vfs_listdir(struct dentry *dir, char *out, size_t cap);

/* Like vfs_listdir but emits "TYPE SIZE NAME\n" rows (ls -l style)
 * where TYPE is one of dir/reg/chr.  SIZE is the file size for reg,
 * or 0 for dir/chr.  Used by SYS_lsl. */
long           vfs_listdir_long(struct dentry *dir, char *out, size_t cap);

/* ---- Inode lifecycle (SVR4 vnode v_count style) -------------------- */

/* Bump i_count to register a new reference (e.g. an open file).
 * Safe on NULL. */
void          vfs_iget(struct inode *ino);

/* Drop a reference.  When i_count falls to 0 the FS's inactive hook
 * is called to release i_private and the inode is freed.  Safe on
 * NULL. */
void          vfs_iput(struct inode *ino);

/* ---- fd table ------------------------------------------------------- */

int           fd_alloc(struct file *f);
void          fd_free(int fd);
struct file  *fd_get(int fd);

/* Close every fd still open in the dying thread.  Called from
 * kthread_exit; declared here so sched.c can call it without pulling
 * the per-thread fdt details into its headers. */
struct kthread;
void          vfs_drain_fds(struct kthread *t);

/* ---- Syscall implementations (dispatch through f_ops) --------------- */

int   sys_open_impl(const char *path, int flags);
int   sys_mkdir_impl(const char *path);
int   sys_unlink_impl(const char *path);

/* Resolve a path (absolute or relative) against the current
 * thread's vm_map->cwd into the supplied buffer.  Returns `out`
 * on success, or NULL if the result wouldn't fit.  Defined in
 * uts/os/proc/syscall.c.  Kernel-internal callers passing
 * absolute paths ("/dev/tty0") get them back unchanged. */
const char *resolve_path_kva(const char *path, char *out, size_t cap);
int   sys_rmdir_impl(const char *path);

/* VFS-level helper: remove the dentry named `name` from `parent`'s
 * child list.  Does not free the inode/dentry (memory leak for now);
 * just unlinks from the tree so lookups fail. */
int   vfs_remove_child(struct dentry *parent, const char *name);
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

int   sys_creat_impl(const char *path);
long  sys_seek_impl (int fd, long offset, int whence);

/* Whence constants matching POSIX. */
#define SEEK_SET	0
#define SEEK_CUR	1
#define SEEK_END	2

#endif
