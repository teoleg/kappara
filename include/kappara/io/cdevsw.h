/*
 * include/kappara/cdevsw.h -- SVR4-style character-device switch
 *
 * In System V Unix, character drivers don't hang their entry points
 * off the inode the way Linux does.  Instead the kernel keeps a
 * cdevsw[] table indexed by major number; each entry holds the
 * driver's entry points (or, for a STREAMS driver, a streamtab *).
 * Opening a chrdev does:
 *
 *     dev_t  rdev = inode->i_rdev;
 *     entry = &cdevsw[MAJOR(rdev)];
 *     entry->open(MINOR(rdev), ...);
 *
 * The minor number lets a single driver back several /dev nodes
 * (think tty0..tty3).  Major number is the index; nothing else.
 *
 * What kappara stores
 * -------------------
 * Each cdev entry holds a name (for diagnostics + /dev mknod) and
 * the streamtab the driver registered with.  When the VFS opens a
 * chrdev inode it pulls the streamtab from cdevsw[major] and asks
 * stream_build to construct the queue stack.  Minor is currently
 * ignored by every driver -- there's only one /dev/console -- but
 * it's plumbed through so adding /dev/tty1 is a one-liner.
 *
 * SVR4 vs Linux
 * -------------
 *   Linux:  struct file_operations *f_op; lives on the inode, every
 *           chrdev is "a file with funny ops."
 *   SVR4:   cdevsw[major] is the registry; the inode just carries
 *           dev_t and the kernel does the lookup.  Drivers don't
 *           know they live on a VFS.
 *
 * We follow SVR4.
 */

#ifndef KAPPARA_CDEVSW_H
#define KAPPARA_CDEVSW_H

#include <stdint.h>

struct streamtab;

/* dev_t encoding: high 8 bits = major, low 24 = minor.  Plenty of
 * room for both at this scale; the split is just for clarity. */
typedef uint32_t dev_t;

#define MAJOR(d)	((unsigned)((d) >> 24))
#define MINOR(d)	((unsigned)((d) & 0x00ffffffu))
#define MKDEV(m, n)	(((dev_t)(m) << 24) | ((dev_t)(n) & 0x00ffffffu))

/* Reserved majors -- bumped as new drivers come up. */
#define CDEV_MAJ_LOOP		1
#define CDEV_MAJ_NULL		2
#define CDEV_MAJ_CONSOLE	3
#define CDEV_MAJ_KLOG		4
#define CDEV_MAJ_UPPER		5
#define CDEV_MAJ_DELAY		6
#define CDEV_MAJ_FBCON		7
#define CDEV_MAJ_PROC_PS	8	/* /proc/ps        */
#define CDEV_MAJ_PROC_MEM	9	/* /proc/meminfo   */
#define CDEV_MAJ_PROC_SLAB	10	/* /proc/slabinfo  */
#define CDEV_MAJ_PROC_STREAM	11	/* /proc/streams   */
#define CDEV_MAJ_PROC_FTRACE	12	/* /proc/ftrace    */
#define CDEV_MAJ_PROC_CPU	13	/* /proc/cpuload   */
#define CDEV_MAJ_TTY		14	/* /dev/tty0..tty3 -- multi-minor */
#define CDEV_MAJ_LO		15	/* /dev/lo0 -- loopback iface     */
#define CDEV_MAJ_ICMP		16	/* /dev/icmp -- ICMP echo device  */
#define CDEV_MAJ_PROC_NETIF	17	/* /proc/netif -- interface list  */
#define CDEV_MAJ_UDP		18	/* /dev/udp -- UDP TPI device     */
#define CDEV_MAJ_TCP		19	/* /dev/tcp -- TCP TPI device     */
#define CDEV_MAJ_PROC_SLIP	20	/* /proc/slip -- slip0 byte/frame counters */
#define CDEV_MAJ_PROC_TCP	21	/* /proc/tcp  -- TCP connection table */
#define CDEV_MAJ_PROC_ACPI	22	/* /proc/acpi -- ACPI table summary  */
#define CDEV_MAJ_PROC_PCI	23	/* /proc/pci  -- PCIe device list    */
#define CDEV_MAJ_PROC_EFI	24	/* /proc/efi  -- EFI memory map      */
#define CDEV_MAJ_PROC_NVME	25	/* /proc/nvme -- NVMe controller info */
#define CDEV_MAJ_PROC_MOUNTS	26	/* /proc/mounts -- live kfs mount table */
#define CDEV_MAJ_KSYMS		27	/* /dev/ksyms -- kernel symbol table   */

#define CDEV_MAX		32

struct cdev_entry {
	const char         *name;	/* for /dev mknod + diagnostics */
	struct streamtab   *streamtab;	/* STREAMS driver entry points  */
};

/* Install `st` at cdevsw[major].  Returns 0 on success, -1 if the
 * slot is taken or out of range.  Modules (push-able mid-stream
 * pieces like "upper" and "delay") don't strictly need a major --
 * they live in the by-name registry for I_PUSH lookup -- but it
 * costs nothing to mknod them under /dev too, so we do. */
int                cdev_register(unsigned major, const char *name,
				  struct streamtab *st);

/* Return the cdev_entry for `major`, or NULL.  Used by the VFS open
 * path to find the streamtab for a chrdev inode. */
struct cdev_entry *cdev_lookup(unsigned major);

#endif
