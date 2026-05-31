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
 *   2. Own the global fd table.  fd_alloc / fd_free / fd_get; small
 *      fixed-size array indexed by fd integer.
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

#include "kappara/kmem.h"
#include "kappara/printk.h"
#include "kappara/string.h"
#include "kappara/uaccess.h"
#include "kappara/vfs.h"

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
				struct file_ops *fops, void *priv)
{
	struct inode *i = new_inode(INODE_CHRDEV, fops, priv);
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
	case INODE_REG:    return "reg";
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

/* ---- fd table --------------------------------------------------------- */

#define FD_MAX	16

static struct file *fd_table[FD_MAX];

int fd_alloc(struct file *f)
{
	for (int i = 0; i < FD_MAX; i++) {
		if (!fd_table[i]) {
			fd_table[i] = f;
			return i;
		}
	}
	return -1;
}

void fd_free(int fd)
{
	if (fd < 0 || fd >= FD_MAX)
		return;
	fd_table[fd] = NULL;
}

struct file *fd_get(int fd)
{
	if (fd < 0 || fd >= FD_MAX)
		return NULL;
	return fd_table[fd];
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
	f->f_ops     = ino->i_fops;
	f->f_inode   = ino;
	f->f_private = NULL;
	f->f_refs    = 1;
	f->f_flags   = flags;

	if (f->f_ops->open(f) < 0) {
		kfree(f);
		return -1;
	}

	int fd = fd_alloc(f);
	if (fd < 0) {
		if (f->f_ops->close)
			f->f_ops->close(f);
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
			*link = cur->d_sibling;
			return 0;
		}
		link = &cur->d_sibling;
	}
	return -1;
}

int sys_close_impl(int fd)
{
	struct file *f = fd_get(fd);
	if (!f)
		return -1;
	if (f->f_ops->close)
		f->f_ops->close(f);
	fd_free(fd);
	kfree(f);
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
