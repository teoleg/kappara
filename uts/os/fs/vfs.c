/*
 * kernel/vfs.c -- in-memory VFS + fd table + file-syscall dispatch
 * ================================================================
 *
 * Three jobs in one file:
 *
 *   1. Manage the in-memory dentry/inode tree (vfs_init, vfs_lookup,
 *      vfs_mkdir, vfs_mknod_chrdev).  Real filesystems will plug in
 *      next to this.
 *
 *   2. Drive the per-thread fd table.  fd_alloc / fd_free / fd_get
 *      operate on cur->fdt[], so each thread sees its own fds; spawn
 *      copies the parent's table over and bumps f_refs on every
 *      inherited file so a close in one thread doesn't free the
 *      file out from under the other.
 *
 *   3. Provide the sys_*_impl entry points that the syscall layer
 *      calls into.  These do the path lookup, find the file_ops via
 *      the inode, and dispatch.  Nothing in here knows anything
 *      about STREAMS -- that's all in stream_head.c behind a
 *      file_ops vtable.
 *
 * Path lookup
 * -----------
 *
 *     vfs_lookup("/dev/loop")
 *
 *     root  /         (INODE_DIR)
 *       |
 *      dev            (INODE_DIR)
 *       |  +-> "loop" component matches a child dentry
 *       v
 *      loop           (INODE_CHRDEV, i_private = &loop_streamtab)
 *
 *   No symlinks, no mounts, no fancy hashing yet -- linear sibling
 *   scan at each level.  The tree is tiny.
 *
 * open flow (in sys_open_impl)
 * ----------------------------
 *
 *   1. vfs_lookup(path)              -- dentry + inode
 *   2. allocate struct file          -- f_ops/f_inode from the inode
 *   3. call f_ops->open(file)        -- per-driver setup; e.g. the
 *                                       stream chrdev's open builds
 *                                       a head + driver stack and
 *                                       stashes stdata* in f_private
 *   4. fd_alloc(file)                -- pick a free fd, install
 *   5. return fd
 *
 * close, read, write, ioctl, putmsg, getmsg all just dispatch
 * through the file's f_ops.  If a method is NULL we return -1 --
 * later we will distinguish errno values (ENOSYS, etc.).
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/core/atomic.h"
#include "kappara/fs/bdevsw.h"
#include "kappara/fs/blkdev.h"
#include "kappara/fs/buf.h"
#include "kappara/io/cdevsw.h"
#include "kappara/core/kmem.h"
#include "kappara/core/printk.h"
#include "kappara/proc/sched.h"
#include "kappara/core/string.h"
#include "kappara/core/uaccess.h"
#include "kappara/fs/vfs.h"

/* The single chrdev file_ops table -- defined in kernel/stream_head.c.
 * All STREAMS chrdev inodes hang their VFS dispatch off this one
 * vtable; the per-driver behavior is reached through cdevsw[MAJOR(rdev)]
 * inside the stream_*  methods.  See include/kappara/io/cdevsw.h. */
extern struct file_ops stream_fops;

/* ---- Root of the in-memory tree --------------------------------------- */

static struct dentry root_dentry;
static struct inode  root_inode;

/* ---- Helpers ---------------------------------------------------------- */

static struct dentry *new_dentry(const char *name, struct inode *inode,
				 struct dentry *parent)
{
	struct dentry *d = kmalloc(sizeof(*d));
	if (!d)
		return NULL;
	d->d_name    = name;
	d->d_inode   = inode;
	d->d_parent  = parent;
	d->d_child   = NULL;
	d->d_sibling = parent ? parent->d_child : NULL;
	if (parent)
		parent->d_child = d;
	return d;
}

static struct inode *new_inode(enum inode_type t, struct file_ops *fops,
			       void *priv)
{
	struct inode *i = kmalloc(sizeof(*i));
	if (!i)
		return NULL;
	i->i_type    = t;
	i->i_fops    = fops;
	i->i_private = priv;
	i->i_rdev    = 0;
	i->i_count   = 1;	/* the dentry that's about to be created
				 * holds this initial reference */
	return i;
}

static struct dentry *find_child(struct dentry *parent,
				 const char *name, size_t len)
{
	for (struct dentry *d = parent->d_child; d; d = d->d_sibling) {
		size_t dn = kstrlen(d->d_name);
		if (dn != len)
			continue;
		size_t i;
		for (i = 0; i < len; i++)
			if (d->d_name[i] != name[i])
				break;
		if (i == len)
			return d;
	}
	return NULL;
}

/* ---- Tree API --------------------------------------------------------- */

void vfs_init(void)
{
	root_inode.i_type    = INODE_DIR;
	root_inode.i_fops    = NULL;
	root_inode.i_private = NULL;

	root_dentry.d_name    = "/";
	root_dentry.d_inode   = &root_inode;
	root_dentry.d_parent  = &root_dentry;
	root_dentry.d_sibling = NULL;
	root_dentry.d_child   = NULL;

	kprintf("vfs: rootfs ready\n");
}

struct dentry *vfs_root(void)
{
	return &root_dentry;
}

struct dentry *vfs_lookup(const char *path)
{
	if (!path || path[0] != '/')
		return NULL;
	struct dentry *cur = &root_dentry;
	const char *p = path + 1;
	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		const char *end = p;
		while (*end && *end != '/')
			end++;
		size_t len = (size_t)(end - p);
		if (cur->d_inode->i_type != INODE_DIR)
			return NULL;
		struct dentry *child = find_child(cur, p, len);
		if (!child)
			return NULL;
		cur = child;
		p = end;
	}
	return cur;
}

struct dentry *vfs_mkdir(struct dentry *parent, const char *name)
{
	struct inode *i = new_inode(INODE_DIR, NULL, NULL);
	return new_dentry(name, i, parent);
}

struct dentry *vfs_mknod_chrdev(struct dentry *parent, const char *name,
				uint32_t rdev)
{
	if (!cdev_lookup(MAJOR(rdev))) {
		kprintf("vfs_mknod_chrdev: '%s': no driver at major %u\n",
			name, MAJOR(rdev));
		return NULL;
	}
	struct inode *i = new_inode(INODE_CHRDEV, &stream_fops, NULL);
	if (!i) return NULL;
	i->i_rdev = rdev;
	return new_dentry(name, i, parent);
}

/* ---- block-device file_ops -- read/write through the buffer cache --- */

/* Per-open cursor: a single byte offset that read/write/seek share. */
struct blkdev_cursor {
	uint64_t pos;
};

static int blkdev_open(struct file *f)
{
	struct blkdev_cursor *c = kmalloc(sizeof(*c));
	if (!c) return -1;
	c->pos = 0;
	f->f_private = c;
	return 0;
}

static int blkdev_close(struct file *f)
{
	if (f->f_private) kfree(f->f_private);
	f->f_private = NULL;
	return 0;
}

/* Walk arbitrary byte ranges by going block-at-a-time through
 * bread.  No alignment assumptions on the user buffer: we copy
 * partial leading/trailing blocks through bp->b_data.  Tail-EOF
 * is signalled by returning 0; partial-success returns the bytes
 * actually transferred. */
static long blkdev_read(struct file *f, void *buf, size_t len)
{
	struct blkdev_cursor *c = f->f_private;
	dev_t dev = f->f_inode->i_rdev;

	size_t done = 0;
	uint8_t *out = buf;
	while (done < len) {
		uint64_t blkno  = c->pos / BLK_SIZE;
		uint32_t inblk  = (uint32_t)(c->pos % BLK_SIZE);
		struct buf *bp = bread(dev, blkno);
		if (!bp) {
			/* Driver bounce -- treat as EOF rather than -1 so
			 * `cat` walks off the end cleanly. */
			break;
		}
		size_t n = BLK_SIZE - inblk;
		if (n > len - done) n = len - done;
		kmemcpy(out + done, (uint8_t *)bp->b_data + inblk, n);
		brelse(bp);
		done   += n;
		c->pos += n;
	}
	return (long)done;
}

static long blkdev_write(struct file *f, const void *buf, size_t len)
{
	struct blkdev_cursor *c = f->f_private;
	dev_t dev = f->f_inode->i_rdev;

	size_t done = 0;
	const uint8_t *in = buf;
	while (done < len) {
		uint64_t blkno  = c->pos / BLK_SIZE;
		uint32_t inblk  = (uint32_t)(c->pos % BLK_SIZE);
		size_t n = BLK_SIZE - inblk;
		if (n > len - done) n = len - done;

		struct buf *bp;
		if (inblk == 0 && n == BLK_SIZE) {
			/* Full-block overwrite: skip the read; just stamp
			 * the cache slot and bwrite it. */
			bp = getblk(dev, blkno);
			if (!bp) break;
			kmemcpy(bp->b_data, in + done, BLK_SIZE);
			bp->b_flags |= B_VALID;
		} else {
			/* Read-modify-write for partial blocks. */
			bp = bread(dev, blkno);
			if (!bp) break;
			kmemcpy((uint8_t *)bp->b_data + inblk, in + done, n);
		}
		if (bwrite(bp) < 0) break;
		done   += n;
		c->pos += n;
	}
	return (long)done;
}

static long blkdev_seek(struct file *f, long offset, int whence)
{
	struct blkdev_cursor *c = f->f_private;
	long newpos;
	switch (whence) {
	case 0: newpos = offset; break;			/* SEEK_SET */
	case 1: newpos = (long)c->pos + offset; break;	/* SEEK_CUR */
	default: return -1;	/* SEEK_END needs a size; later */
	}
	if (newpos < 0) return -1;
	c->pos = (uint64_t)newpos;
	return newpos;
}

static struct file_ops blkdev_fops = {
	.open  = blkdev_open,
	.close = blkdev_close,
	.read  = blkdev_read,
	.write = blkdev_write,
	.seek  = blkdev_seek,
};

struct dentry *vfs_mknod_blkdev(struct dentry *parent, const char *name,
				uint32_t rdev)
{
	if (!bdev_lookup(MAJOR(rdev))) {
		kprintf("vfs_mknod_blkdev: '%s': no driver at major %u\n",
			name, MAJOR(rdev));
		return NULL;
	}
	struct inode *i = new_inode(INODE_BLOCKDEV, &blkdev_fops, NULL);
	if (!i) return NULL;
	i->i_rdev = rdev;
	return new_dentry(name, i, parent);
}

struct dentry *vfs_mknod_regfile(struct dentry *parent, const char *name,
				 struct file_ops *fops, void *priv)
{
	struct inode *i = new_inode(INODE_REG, fops, priv);
	return new_dentry(name, i, parent);
}

/* ---- Visualisation --------------------------------------------------- */

static const char *type_tag(enum inode_type t)
{
	switch (t) {
	case INODE_DIR:    return "dir";
	case INODE_CHRDEV: return "chr";
	case INODE_REG:     return "reg";
	case INODE_BLOCKDEV: return "blk";
	default:           return "?";
	}
}

static void dump_one(struct dentry *d, int depth)
{
	for (int i = 0; i < depth; i++)
		kprintf("  ");

	int is_dir   = d->d_inode && d->d_inode->i_type == INODE_DIR;
	int name_is_root = (d->d_name[0] == '/' && d->d_name[1] == '\0');

	kprintf("%s%s  [%s]\n",
		d->d_name,
		(is_dir && !name_is_root) ? "/" : "",
		d->d_inode ? type_tag(d->d_inode->i_type) : "?");

	if (is_dir) {
		for (struct dentry *c = d->d_child; c; c = c->d_sibling)
			dump_one(c, depth + 1);
	}
}

void vfs_dump_tree(struct dentry *d)
{
	if (!d) d = vfs_root();
	kprintf("filesystem:\n");
	dump_one(d, 0);
}

long vfs_listdir(struct dentry *dir, char *out, size_t cap)
{
	if (!dir || !dir->d_inode || dir->d_inode->i_type != INODE_DIR)
		return -1;
	size_t off = 0;
	for (struct dentry *c = dir->d_child; c; c = c->d_sibling) {
		size_t nlen = kstrlen(c->d_name);
		if (off + nlen + 1 > cap)
			break;	/* truncate */
		for (size_t i = 0; i < nlen; i++)
			out[off + i] = c->d_name[i];
		off += nlen;
		out[off++] = '\n';
	}
	return (long)off;
}

static const char *type_tag_short(enum inode_type t)
{
	switch (t) {
	case INODE_DIR:    return "dir";
	case INODE_CHRDEV: return "chr";
	case INODE_REG:     return "reg";
	case INODE_BLOCKDEV: return "blk";
	}
	return "?";
}

/* Append `s` to out[off..cap], return new off (clipped to cap). */
static size_t append_str(char *out, size_t off, size_t cap, const char *s)
{
	while (*s && off + 1 < cap) out[off++] = *s++;
	return off;
}

/* Append unsigned decimal v (right-justified to width) to out. */
static size_t append_dec(char *out, size_t off, size_t cap,
			 unsigned long v, int width)
{
	char tmp[24];
	int  i = 0;
	if (v == 0) tmp[i++] = '0';
	while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
	while (i < width && off + 1 < cap) { out[off++] = ' '; width--; }
	while (i-- && off + 1 < cap) out[off++] = tmp[i];
	return off;
}

long vfs_listdir_long(struct dentry *dir, char *out, size_t cap)
{
	if (!dir || !dir->d_inode || dir->d_inode->i_type != INODE_DIR)
		return -1;
	size_t off = 0;
	for (struct dentry *c = dir->d_child; c; c = c->d_sibling) {
		struct inode *ino = c->d_inode;
		const char *tag = ino ? type_tag_short(ino->i_type) : "?";
		off = append_str(out, off, cap, tag);
		out[off++] = ' ';
		/* Real Unix ls -l replaces the size column with "M, N"
		 * for character special files -- the major+minor dev_t
		 * tuple that selects the driver in cdevsw[].  Same here. */
		if (ino && (ino->i_type == INODE_CHRDEV ||
			    ino->i_type == INODE_BLOCKDEV)) {
			off = append_dec(out, off, cap, MAJOR(ino->i_rdev), 4);
			out[off++] = ',';
			off = append_dec(out, off, cap, MINOR(ino->i_rdev), 3);
		} else {
			long sz = 0;
			if (ino && ino->i_fops && ino->i_fops->size)
				sz = ino->i_fops->size(ino);
			if (sz < 0) sz = 0;
			off = append_dec(out, off, cap, (unsigned long)sz, 8);
		}
		out[off++] = ' ';
		off = append_str(out, off, cap, c->d_name);
		if (off + 1 >= cap) break;
		out[off++] = '\n';
	}
	return (long)off;
}

/* ---- fd table --------------------------------------------------------- */

/* Per-thread fdt lives in struct kthread (KT_FD_MAX slots).  The
 * symbols below operate on whichever thread is currently scheduled,
 * which is the right thing for the syscall path. */

int fd_alloc(struct file *f)
{
	struct kthread *me = curthread;
	if (!me) return -1;
	for (int i = 0; i < KT_FD_MAX; i++) {
		if (!me->fdt[i]) {
			me->fdt[i] = f;
			return i;
		}
	}
	return -1;
}

void fd_free(int fd)
{
	struct kthread *me = curthread;
	if (!me || fd < 0 || fd >= KT_FD_MAX)
		return;
	me->fdt[fd] = NULL;
}

struct file *fd_get(int fd)
{
	struct kthread *me = curthread;
	if (!me || fd < 0 || fd >= KT_FD_MAX)
		return NULL;
	return me->fdt[fd];
}

/* ---- struct file reference counting -----------------------------------
 *
 * f_refs counts every fdt slot (across every thread) that still names
 * this struct file.  Threads share files via fork-style fd inheritance
 * (kthread_inherit_fds), so the same struct file is reachable from
 * multiple CPUs at once and concurrent close() / dup() races on the
 * counter.
 *
 * Discipline:
 *   * Initialisation: plain store of 1 at struct creation time,
 *     BEFORE the pointer is published to a fdt slot.  Safe because
 *     no other CPU can see f yet.
 *   * Incrementing an existing reference (dup, fd inherit): file_get(f).
 *   * Decrementing a reference: file_put(f).  When file_put drives
 *     the count to zero it is the unique cleanup observer and runs
 *     close + vfs_iput + kfree.
 *
 * Both helpers route through atomic_inc / atomic_dec_and_test in
 * include/kappara/core/atomic.h -- never write raw `f->f_refs++` or
 * `--f->f_refs`, those lose updates under SMP and have driven at
 * least one panic into this tree already. */
static void file_get(struct file *f)
{
	if (!f) return;
	atomic_inc(&f->f_refs);
}

static void file_put(struct file *f)
{
	if (!f) return;
	if (!atomic_dec_and_test(&f->f_refs))
		return;
	/* We drove f_refs from 1 to 0; nobody else can reach *f. */
	if (f->f_ops && f->f_ops->close)
		f->f_ops->close(f);
	/* Drop the inode reference acquired by sys_open_impl.  If this
	 * was the last reference -- and the name has already been
	 * unlinked -- vop_inactive runs now and the inode is freed. */
	vfs_iput(f->f_inode);
	kfree(f);
}

void kthread_inherit_fds(struct kthread *child, const struct kthread *parent)
{
	if (!child || !parent) return;
	/* Parent's fdt is only touched by the parent thread (== curthread,
	 * the caller).  Child isn't dispatched yet so its fdt has no
	 * other observers.  The only piece that needs atomicity is the
	 * shared f_refs counter -- different exec'd / spawned children
	 * across CPUs race here. */
	for (int i = 0; i < KT_FD_MAX; i++) {
		struct file *f = parent->fdt[i];
		child->fdt[i] = f;
		file_get(f);		/* no-op on NULL */
	}
}

/* Called from kthread_exit on the dying thread.  Releases every fd
 * the thread still holds, so exiting cleanly is enough to close any
 * pipe ends or open files the thread inherited or opened. */
void vfs_drain_fds(struct kthread *t)
{
	if (!t) return;
	for (int i = 0; i < KT_FD_MAX; i++) {
		struct file *f = t->fdt[i];
		if (!f) continue;
		t->fdt[i] = NULL;
		file_put(f);
	}
}

/* ---- Syscall entry points -------------------------------------------- */

int sys_open_impl(const char *path, int flags)
{
	char kpath[128];
	const char *p;
	if (syscall_from_user) {
		if (strncpy_from_user(kpath, path, sizeof(kpath)) < 0) {
			kprintf("sys_open: rejected user path pointer\n");
			return -1;
		}
		p = kpath;
	} else {
		if (!path)
			return -1;
		p = path;
	}
	/* Resolve "." / "nvme" / "./foo" against the calling process's
	 * cwd before vfs_lookup, which only understands absolute
	 * paths.  Kernel-internal callers passing "/dev/tty0" et al
	 * are absolute and pass through unchanged. */
	char resolved[128];
	const char *rp = resolve_path_kva(p, resolved, sizeof(resolved));
	if (!rp) {
		kprintf("sys_open: path too long after cwd join: '%s'\n", p);
		return -1;
	}
	p = rp;
	struct dentry *d = vfs_lookup(p);
	if (!d) {
		kprintf("sys_open: ENOENT '%s'\n", p);
		return -1;
	}
	struct inode *ino = d->d_inode;
	if (!ino->i_fops || !ino->i_fops->open) {
		kprintf("sys_open: '%s' has no open op\n", p);
		return -1;
	}

	struct file *f = kmalloc(sizeof(*f));
	if (!f)
		return -1;
	/* Pin the inode for as long as the open file holds it.  The
	 * matching vfs_iput is in file_put when the last close drops
	 * f_refs to 0 -- unlink between now and then sees i_count > 1
	 * and only removes the name. */
	vfs_iget(ino);
	f->f_ops     = ino->i_fops;
	f->f_inode   = ino;
	f->f_private = NULL;
	f->f_refs    = 1;
	f->f_flags   = flags;

	if (f->f_ops->open(f) < 0) {
		vfs_iput(ino);
		kfree(f);
		return -1;
	}

	int fd = fd_alloc(f);
	if (fd < 0) {
		if (f->f_ops->close)
			f->f_ops->close(f);
		vfs_iput(ino);
		kfree(f);
		return -1;
	}
	return fd;
}

/*
 * Resolve a path into (parent dentry, basename).  Used by every
 * "operate on a name in a directory" syscall (creat, mkdir, unlink,
 * rmdir).  Copies the user pointer if syscall_from_user is set;
 * writes the parent name into dirbuf and points *base into the
 * original (kernel-side) string.
 */
static int resolve_parent(const char *path, char *dirbuf, size_t dirsz,
			  const char **base_out, char *pathbuf,
			  size_t pathsz)
{
	const char *p;
	if (syscall_from_user) {
		if (strncpy_from_user(pathbuf, path, pathsz) < 0)
			return -1;
		p = pathbuf;
	} else {
		if (!path) return -1;
		p = path;
	}
	if (p[0] != '/') return -1;

	const char *last = p;
	for (const char *q = p; *q; q++)
		if (*q == '/') last = q;
	size_t dirlen = (size_t)(last - p);
	if (dirlen == 0) dirlen = 1;
	if (dirlen >= dirsz) return -1;
	for (size_t i = 0; i < dirlen; i++) dirbuf[i] = p[i];
	dirbuf[dirlen] = '\0';
	*base_out = last + 1;
	return 0;
}

int sys_mkdir_impl(const char *path)
{
	char dirbuf[128], pathbuf[128];
	const char *base;
	if (resolve_parent(path, dirbuf, sizeof(dirbuf), &base,
			   pathbuf, sizeof(pathbuf)) < 0) {
		kprintf("sys_mkdir: bad path\n");
		return -1;
	}
	if (!*base) return -1;
	struct dentry *d = vfs_lookup(dirbuf);
	if (!d || !d->d_inode) return -1;
	struct inode *parent = d->d_inode;
	if (parent->i_type != INODE_DIR || !parent->i_fops ||
	    !parent->i_fops->mkdir)
		return -1;
	return parent->i_fops->mkdir(parent, base);
}

int sys_unlink_impl(const char *path)
{
	char dirbuf[128], pathbuf[128];
	const char *base;
	if (resolve_parent(path, dirbuf, sizeof(dirbuf), &base,
			   pathbuf, sizeof(pathbuf)) < 0)
		return -1;
	if (!*base) return -1;
	struct dentry *d = vfs_lookup(dirbuf);
	if (!d || !d->d_inode) return -1;
	struct inode *parent = d->d_inode;
	if (!parent->i_fops || !parent->i_fops->unlink) {
		kprintf("sys_unlink: '%s' has no unlink op\n", dirbuf);
		return -1;
	}
	int rc = parent->i_fops->unlink(parent, base);
	if (rc == 0)
		vfs_remove_child(d, base);
	return rc;
}

int sys_rmdir_impl(const char *path)
{
	char dirbuf[128], pathbuf[128];
	const char *base;
	if (resolve_parent(path, dirbuf, sizeof(dirbuf), &base,
			   pathbuf, sizeof(pathbuf)) < 0)
		return -1;
	if (!*base) return -1;
	struct dentry *d = vfs_lookup(dirbuf);
	if (!d || !d->d_inode) return -1;
	struct inode *parent = d->d_inode;
	if (!parent->i_fops || !parent->i_fops->rmdir) {
		kprintf("sys_rmdir: '%s' has no rmdir op\n", dirbuf);
		return -1;
	}
	int rc = parent->i_fops->rmdir(parent, base);
	if (rc == 0)
		vfs_remove_child(d, base);
	return rc;
}

int vfs_remove_child(struct dentry *parent, const char *name)
{
	if (!parent || !parent->d_child) return -1;
	struct dentry **link = &parent->d_child;
	while (*link) {
		struct dentry *cur = *link;
		size_t i;
		for (i = 0; cur->d_name[i] && cur->d_name[i] == name[i]; i++)
			;
		if (cur->d_name[i] == '\0' && name[i] == '\0') {
			/* Splice out of the parent's child list. */
			*link = cur->d_sibling;
			/* Drop the dentry's reference to its inode --
			 * if no open file is holding it the inode goes
			 * away inside vfs_iput (vop_inactive frees
			 * i_private, then we kfree the inode).  Anyone
			 * who still has an fd on this inode keeps it
			 * alive until they close, matching the SVR4
			 * "removed-but-open" lifetime rule. */
			vfs_iput(cur->d_inode);
			/* d_name was kmalloc'd via kfs_dup_name when the
			 * dirent was added (literal-name dentries like
			 * the root or /dev are never passed to
			 * vfs_remove_child, so this is always slab
			 * memory). */
			kfree((void *)(uintptr_t)cur->d_name);
			kfree(cur);
			return 0;
		}
		link = &cur->d_sibling;
	}
	return -1;
}

/* ---- struct inode reference counting ---------------------------------
 *
 * i_count is the SVR4 v_count: one reference per dentry that names
 * the inode plus one per open struct file that has it.  Multi-CPU
 * concurrency arises in the same way as f_refs -- two threads each
 * closing the last file that holds the inode will race the
 * decrement.  Without atomicity one of the drops is lost (leak) or
 * both observers think they did the last drop and vop_inactive +
 * kfree runs twice (double free).  Route through atomic_inc /
 * atomic_dec_and_test; never write raw `++` / `--`. */
void vfs_iget(struct inode *ino)
{
	if (!ino) return;
	atomic_inc(&ino->i_count);
}

void vfs_iput(struct inode *ino)
{
	if (!ino) return;
	if (!atomic_dec_and_test(&ino->i_count))
		return;
	/* Last reference -- ask the FS to release whatever lived behind
	 * i_private (kfs_file, kfs_dir), then free the inode body. */
	if (ino->i_fops && ino->i_fops->inactive)
		ino->i_fops->inactive(ino);
	kfree(ino);
}

int sys_close_impl(int fd)
{
	struct file *f = fd_get(fd);
	if (!f)
		return -1;
	fd_free(fd);
	file_put(f);
	return 0;
}

long sys_read_impl(int fd, void *buf, size_t len)
{
	if (syscall_from_user && !user_ptr_ok(buf, len)) {
		kprintf("sys_read: rejected user buf %p len %lu\n",
			buf, (unsigned long)len);
		return -1;
	}
	struct file *f = fd_get(fd);
	if (!f || !f->f_ops->read)
		return -1;
	return f->f_ops->read(f, buf, len);
}

long sys_write_impl(int fd, const void *buf, size_t len)
{
	if (syscall_from_user && !user_ptr_ok(buf, len)) {
		kprintf("sys_write: rejected user buf %p len %lu\n",
			buf, (unsigned long)len);
		return -1;
	}
	struct file *f = fd_get(fd);
	if (!f || !f->f_ops->write)
		return -1;
	return f->f_ops->write(f, buf, len);
}

long sys_ioctl_impl(int fd, int cmd, long arg)
{
	struct file *f = fd_get(fd);
	if (!f || !f->f_ops->ioctl)
		return -1;

	/* I_PUSH takes a (user) string pointer.  Copy the name into a
	 * local buffer so the ioctl handler can dereference safely
	 * without trusting the user pointer beyond a bounds check.
	 * I_POP / future integer ioctls pass through unchanged. */
	char kname[64];
	if (syscall_from_user && cmd == 1 /* I_PUSH */) {
		if (strncpy_from_user(kname,
				      (const char *)(uintptr_t)arg,
				      sizeof(kname)) < 0) {
			kprintf("sys_ioctl(I_PUSH): rejected user name ptr\n");
			return -1;
		}
		arg = (long)(uintptr_t)kname;
	}
	return f->f_ops->ioctl(f, cmd, arg);
}

long sys_putmsg_impl(int fd, const struct strbuf *c,
		     const struct strbuf *d, int flags)
{
	struct file *f = fd_get(fd);
	if (!f || !f->f_ops->putmsg)
		return -1;
	return f->f_ops->putmsg(f, c, d, flags);
}

long sys_getmsg_impl(int fd, struct strbuf *c,
		     struct strbuf *d, int *flagsp)
{
	struct file *f = fd_get(fd);
	if (!f || !f->f_ops->getmsg)
		return -1;
	return f->f_ops->getmsg(f, c, d, flagsp);
}

long sys_seek_impl(int fd, long offset, int whence)
{
	struct file *f = fd_get(fd);
	if (!f || !f->f_ops->seek)
		return -1;
	return f->f_ops->seek(f, offset, whence);
}

/*
 * Split a path like "/etc/foo" into the parent dentry "/etc" and the
 * basename "foo".  Returns 0 on success.  Lives at the bottom of
 * sys_creat_impl since that's the only caller today.
 */
static int split_parent(const char *path, char *dir, size_t dirsz,
			const char **base)
{
	if (!path || path[0] != '/')
		return -1;
	const char *last = path;
	for (const char *p = path; *p; p++)
		if (*p == '/')
			last = p;
	size_t dirlen = (size_t)(last - path);
	if (dirlen == 0) dirlen = 1;	/* parent is "/" itself */
	if (dirlen >= dirsz)
		return -1;
	for (size_t i = 0; i < dirlen; i++)
		dir[i] = path[i];
	dir[dirlen] = '\0';
	*base = last + 1;
	return 0;
}

int sys_creat_impl(const char *path)
{
	char kpath[128];
	const char *p;
	if (syscall_from_user) {
		if (strncpy_from_user(kpath, path, sizeof(kpath)) < 0) {
			kprintf("sys_creat: rejected user path\n");
			return -1;
		}
		p = kpath;
	} else {
		if (!path) return -1;
		p = path;
	}

	char dirbuf[128];
	const char *basename;
	if (split_parent(p, dirbuf, sizeof(dirbuf), &basename) < 0) {
		kprintf("sys_creat: bad path '%s'\n", p);
		return -1;
	}
	if (!*basename) {
		kprintf("sys_creat: empty basename\n");
		return -1;
	}

	struct dentry *d = vfs_lookup(dirbuf);
	if (!d || !d->d_inode) {
		kprintf("sys_creat: parent '%s' not found\n", dirbuf);
		return -1;
	}
	struct inode *parent = d->d_inode;
	if (parent->i_type != INODE_DIR || !parent->i_fops ||
	    !parent->i_fops->creat) {
		kprintf("sys_creat: '%s' does not support create\n", dirbuf);
		return -1;
	}
	return parent->i_fops->creat(parent, basename);
}
