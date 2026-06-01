/*
 * kernel/main.c -- AArch64 kmain orchestration
 * ============================================
 *
 * The "top-level" of the kernel after boot.S has done its part.
 * Brings each subsystem up in dependency order, then drops into a
 * round-robin yield loop with the timer driving preemption.
 *
 *   uart_init    -- earliest, so subsequent kprintf() works
 *   trap_init    -- install VBAR_EL1 so faults dump instead of looping
 *   mmu_init     -- identity-map the address space, turn on caches
 *   pmm_init     -- enrol the free RAM range with the page allocator
 *   kmem_init    -- prepare the size-caches for kmalloc()
 *   sched_init   -- make this code path tid 0 ("main")
 *   timer_init   -- arm the generic timer at 100 Hz
 *   create demo threads
 *   msr daifclr, #2  -- finally unmask IRQs; the timer now preempts us
 *
 * Two demo threads (alpha and beta) and kmain itself loop printing
 * a counter with a busy spin in between.  Because the spin takes
 * roughly 1 ms and the tick is 10 ms, each thread runs ~7 iterations
 * per slice and the output cycles main / alpha / beta / main / ...
 */

#include <stdint.h>

#include "kappara/blkdev.h"
#include "kappara/fbcon.h"
#include "kappara/framebuffer.h"
#include "kappara/ftrace.h"
#include "kappara/ipi.h"
#include "kappara/kallsyms.h"
#include "kappara/kfs.h"
#include "kappara/kmem.h"
#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/proc.h"
#include "kappara/sched.h"
#include "kappara/stream_head.h"
#include "kappara/streams.h"
#include "kappara/string.h"
#include "kappara/syscall.h"
#include "kappara/vfs.h"
#include "kappara/timer.h"
#include "kappara/trap.h"
#include "kappara/uart.h"
#include "kappara/user.h"
#include "platform.h"

extern char __kernel_end[];

/* Upper bound for pmm.  PLAT_RAM_END is the start of the SoC's
 * peripheral window (0x3F000000 on Pi 3, 0xFE000000 on Pi 4); the
 * GPU's memory split lives just below it.  pmm_carve_gpu() trims
 * this further once the firmware tells us where the framebuffer
 * actually landed.  See discover_gpu_reserve(). */
#define AARCH64_RAM_END	PLAT_RAM_END

/* Find the top of usable RAM after carving out everything the GPU
 * owns.  On a real Pi the firmware reserves the top N MiB of RAM for
 * itself (config.txt's `gpu_mem=`); the framebuffer is allocated out
 * of that block, so its PA is a safe lower bound for "GPU territory".
 * Anything we hand to pmm above that boundary could be silently
 * overwritten by the firmware -- which is the exact bug that makes
 * "boots in QEMU, crashes on Pi" so popular.
 *
 * QEMU's raspi3b honours the same mailbox protocol and returns a PA
 * inside the modelled RAM (typically 0x3c100000 for a 1024x768 FB),
 * so the same code path exercises the carve in the emulator. */
static uintptr_t discover_gpu_reserve(void)
{
	uintptr_t end = AARCH64_RAM_END;
	if (framebuffer_init(1024, 768) == 0) {
		const struct framebuffer *fb = framebuffer_get();
		uintptr_t fb_pa = (uintptr_t)fb->fb;
		if (fb_pa && fb_pa < end) {
			end = fb_pa & ~(PAGE_SIZE - 1);
			kprintf("pmm: GPU owns [0x%lx..0x%lx); trimming pmm "
				"upper bound from 0x%lx to 0x%lx\n",
				(unsigned long)fb_pa,
				(unsigned long)AARCH64_RAM_END,
				(unsigned long)AARCH64_RAM_END,
				(unsigned long)end);
		}
	} else {
		kprintf("pmm: framebuffer_init failed; assuming no GPU "
			"reserve (pmm cap = 0x%lx)\n",
			(unsigned long)end);
	}
	return end;
}

static unsigned current_el(void)
{
	unsigned long el;
	__asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
	return (unsigned)((el >> 2) & 3u);
}

/*
 * The original kernel-mode SVC demo and the alpha/beta busy-printer
 * threads were dropped in favour of the interactive ksh shell.
 * Everything they exercised (svc dispatch, fd table, /dev/loop, I_PUSH,
 * delay-srvp, putmsg/getmsg) is now reachable by typing commands at
 * the kappara# prompt.
 */
#if 0
static void syscall_demo(void)
{
	do_syscall(SYS_log, (long)(uintptr_t)"hello from kernel-svc!", 0, 0);
	long pid = do_syscall(SYS_getpid, 0, 0, 0);
	kprintf("syscall: getpid -> %ld\n", pid);

	/* sys_ls("/dev") via syscall -- prove path lookup works through SVC. */
	char lsbuf[128];
	long lsn = do_syscall(SYS_ls,
			      (long)(uintptr_t)"/dev",
			      (long)(uintptr_t)lsbuf,
			      sizeof(lsbuf));
	kprintf("syscall: ls /dev (%ld bytes):\n", lsn);
	for (long i = 0; i < lsn; i++)
		uart_putc(lsbuf[i]);

	/* Non-existent path: sys_open should fail cleanly. */
	long fmiss = do_syscall(SYS_open, (long)(uintptr_t)"/dev/nope", 0, 0);
	kprintf("syscall: open(\"/dev/nope\") -> %ld (expect -1)\n", fmiss);

	/* STREAMS-over-syscalls: open the "/dev/loop" driver, write a payload,
	 * read it back.  Then I_PUSH "upper" and watch the same payload
	 * come back uppercased.  Then I_POP and confirm lower-case again. */
	long fd = do_syscall(SYS_open, (long)(uintptr_t)"/dev/loop", 0, 0);
	kprintf("syscall: open(\"/dev/loop\") -> %ld\n", fd);

	char buf[32];
	const char msg1[] = "hello stream";
	do_syscall(SYS_write, fd, (long)(uintptr_t)msg1, sizeof(msg1) - 1);
	long n = do_syscall(SYS_read, fd, (long)(uintptr_t)buf, sizeof(buf));
	print_buf("syscall: read (loop only)", buf, n);

	long r = do_syscall(SYS_ioctl, fd, I_PUSH, (long)(uintptr_t)"upper");
	kprintf("syscall: ioctl(I_PUSH, \"upper\") -> %ld\n", r);

	const char msg2[] = "now in upper case please";
	do_syscall(SYS_write, fd, (long)(uintptr_t)msg2, sizeof(msg2) - 1);
	n = do_syscall(SYS_read, fd, (long)(uintptr_t)buf, sizeof(buf));
	print_buf("syscall: read (after I_PUSH upper)", buf, n);

	r = do_syscall(SYS_ioctl, fd, I_POP, 0);
	kprintf("syscall: ioctl(I_POP) -> %ld\n", r);

	const char msg3[] = "back to plain";
	do_syscall(SYS_write, fd, (long)(uintptr_t)msg3, sizeof(msg3) - 1);
	n = do_syscall(SYS_read, fd, (long)(uintptr_t)buf, sizeof(buf));
	print_buf("syscall: read (after I_POP)", buf, n);

	do_syscall(SYS_close, fd, 0, 0);
	kprintf("syscall: close(%ld) done\n", fd);

	/* ----- delay module + service procedure demo --------------------- */
	kprintf("\n--- delay module (service-proc deferred I/O) ---\n");
	fd = do_syscall(SYS_open, (long)(uintptr_t)"/dev/loop", 0, 0);
	do_syscall(SYS_ioctl, fd, I_PUSH, (long)(uintptr_t)"delay");

	const char dmsg[] = "via delay";
	do_syscall(SYS_write, fd, (long)(uintptr_t)dmsg, sizeof(dmsg) - 1);

	n = do_syscall(SYS_read, fd, (long)(uintptr_t)buf, sizeof(buf));
	kprintf("syscall: immediate read = %ld bytes (queued in delay)\n", n);

	/* Two yields: drain delay_wq, then delay_rq. */
	do_syscall(SYS_yield, 0, 0, 0);
	do_syscall(SYS_yield, 0, 0, 0);

	n = do_syscall(SYS_read, fd, (long)(uintptr_t)buf, sizeof(buf));
	print_buf("syscall: read after two yields", buf, n);
	do_syscall(SYS_close, fd, 0, 0);

	/* ----- putmsg / getmsg demo ------------------------------------- */
	kprintf("\n--- putmsg / getmsg (M_PROTO + M_DATA) ---\n");
	fd = do_syscall(SYS_open, (long)(uintptr_t)"/dev/loop", 0, 0);

	const char ctlbytes[]  = "ctl-hdr";
	const char databytes[] = "payload bytes here";
	struct strbuf pctl  = { .maxlen = 0,
				.len    = (int)sizeof(ctlbytes) - 1,
				.buf    = (void *)ctlbytes };
	struct strbuf pdata = { .maxlen = 0,
				.len    = (int)sizeof(databytes) - 1,
				.buf    = (void *)databytes };
	long r2 = do_syscall_4(SYS_putmsg, fd,
			       (long)(uintptr_t)&pctl,
			       (long)(uintptr_t)&pdata, 0);
	kprintf("syscall: putmsg -> %ld\n", r2);

	char gctlbuf[16], gdatabuf[32];
	struct strbuf gctl  = { .maxlen = sizeof(gctlbuf),  .len = 0,
				.buf = gctlbuf  };
	struct strbuf gdata = { .maxlen = sizeof(gdatabuf), .len = 0,
				.buf = gdatabuf };
	int gflags = 0;
	r2 = do_syscall_4(SYS_getmsg, fd,
			  (long)(uintptr_t)&gctl,
			  (long)(uintptr_t)&gdata,
			  (long)(uintptr_t)&gflags);
	kprintf("syscall: getmsg -> %ld (flags=%d)\n", r2, gflags);
	print_buf("  ctl ", gctlbuf,  gctl.len);
	print_buf("  data", gdatabuf, gdata.len);

	do_syscall(SYS_close, fd, 0, 0);
}
#endif

/* ---- SMP foundation -------------------------------------------------
 *
 * boot.S puts secondary cores in a spin-table loop on smp_release[N];
 * writing a non-zero entry + DSB + SEV wakes core N and makes it jump
 * to that address with sp=smp_stacks[N].  For this commit "wake" just
 * means "boot the secondary into a hello-then-idle C function" --
 * full SMP scheduling (per-CPU cur, ready-queue lock, IPI) is the
 * follow-up.
 *
 * The spin-table entries are accessed by the secondary core with its
 * MMU OFF, so writes from core 0 (cached) need an explicit dc cvac +
 * dsb sy to be visible.  Once each secondary has its MMU on, this
 * goes away.
 */

extern uint64_t smp_release[4];
extern uint64_t smp_stacks[4];
extern void     secondary_start(void);	/* boot.S */

/*
 * Per-secondary-CPU C entry.  Called from secondary_start (boot.S)
 * at EL1 with DAIF all masked; cpu is the MPIDR Aff0 field (1..3).
 *
 * Bring-up order mirrors kmain for core 0:
 *   1. MMU + caches on  (same tables core 0 built; just enable for us)
 *   2. VBAR_EL1 installed so our exceptions land on the right vectors
 *   3. Per-CPU scheduler state: struct cpu, idle kthread, TPIDR_EL1
 *   4. Unmask IRQs -- timer ticks may now preempt us
 *   5. Idle loop: yield (giving the scheduler a steal opportunity)
 *      then WFI when there is nothing to run
 */
void secondary_main(unsigned cpu)
{
	mmu_enable_this_cpu();
	trap_init();
	sched_secondary_init(cpu);
	timer_init_this_cpu();	/* arm CNTPNSIRQ + route to this core's IRQ */
	ipi_init_this_cpu();	/* enable mailbox 0 -> IRQ */
	__asm__ volatile ("msr daifclr, #2");
	kprintf("smp: cpu %u entering scheduler\n", cpu);
	for (;;) {
		kthread_yield();
		__asm__ volatile ("wfi");
	}
}

static void smp_dc_cvac(void *p)
{
	uintptr_t a = (uintptr_t)p & ~63UL;
	__asm__ volatile ("dc cvac, %0" :: "r"(a) : "memory");
}

/* Standard ARM spin-table addresses for raspi3b in QEMU (also the
 * BCM2837 convention).  Each core polls its slot at the listed PA;
 * writing a non-zero address there + DSB + SEV releases it.
 *
 *   PA 0xE0 -- core 1 release address
 *   PA 0xE8 -- core 2 release address
 *   PA 0xF0 -- core 3 release address
 *
 * Our boot.S also has its own .Lpark spin-table (used in case a
 * secondary core ever DOES reach our code -- e.g. a future PSCI
 * boot path), but on QEMU the cores never get there; they wait at
 * the QEMU spin-table from reset.  We write both for robustness. */
#define SMP_SPINTABLE_BASE	0xE0UL

/* Wake one secondary core.  Allocates a kernel stack for it,
 * writes the per-core release address + stack-top into both our
 * own spin-table and QEMU's, flushes the lines so the secondary
 * (MMU/cache off) sees them, then DSB SY + SEV. */
void smp_wake_secondary(unsigned cpu)
{
	if (cpu == 0 || cpu >= 4) return;
	void *stack = pmm_alloc();
	if (!stack) {
		kprintf("smp: pmm_alloc failed for cpu %u\n", cpu);
		return;
	}
	uint64_t entry = (uintptr_t)secondary_start;
	uint64_t top   = (uintptr_t)stack + PAGE_SIZE;

	/* Our own spin-table (boot.S .Lpark reads from this).  Harmless
	 * on QEMU since secondaries don't reach it. */
	smp_stacks[cpu]  = top;
	smp_release[cpu] = entry;
	smp_dc_cvac(&smp_release[cpu]);
	smp_dc_cvac(&smp_stacks[cpu]);

	/* QEMU/BCM2837 spin-table at PA 0xE0 + (cpu-1)*8.  This is
	 * where the cores are actually waiting. */
	volatile uint64_t *qemu_slot =
		(volatile uint64_t *)(SMP_SPINTABLE_BASE + (cpu - 1) * 8);
	*qemu_slot = entry;
	smp_dc_cvac((void *)qemu_slot);

	__asm__ volatile ("dsb sy; sev" ::: "memory");
}

void kmain(void)
{
	uart_init();
	trap_init();

	/* Arm the function tracer as early as possible.  ftrace_init
	 * only touches BSS (zeroed by boot.S) and is safe pre-mmu/
	 * pre-pmm.  When the kernel is built without TRACE=1 the
	 * cyg-profile hooks are never emitted, so this is a no-op
	 * apart from zeroing four small ring buffers. */
	ftrace_init();

	kprintf("\nkappara: hello from aarch64\n");
	kprintf("        no soup for you, only streams\n");
	kprintf("        running at EL%u on %s\n",
		current_el(), PLAT_NAME);

	mmu_init();

	/* framebuffer_init must run before pmm_init so we can exclude
	 * the GPU's reserved region from the freelist.  See the comment
	 * on discover_gpu_reserve(). */
	uintptr_t pmm_end = discover_gpu_reserve();
	pmm_init((uintptr_t)__kernel_end, pmm_end);
	kmem_init();

	vfs_init();
	streams_head_init();
	proc_init();
	user_init();

	/* Boot a ramdisk-backed kfs and mount it at /etc.
	 * The mkimage call writes the canned files into the ramdisk
	 * the first time (no formatter tool yet), then mount discovers
	 * them via the block-device interface as if from real storage. */
	ramdisk_init();
	kfs_mkimage(ramdisk_get());
	struct dentry *etc = vfs_mkdir(vfs_root(), "etc");
	kfs_mount(ramdisk_get(), etc);

	/* Draw the boot splash now that everything else is up.  The FB
	 * was already negotiated by discover_gpu_reserve(); if that
	 * succeeded, framebuffer_get() returns the descriptor. */
	if (framebuffer_get()) {
		/* Splash:
		 *   - navy background
		 *   - "kappara" centered near the top at 8x scale (64-pixel
		 *     tall glyphs, so the whole word is 7*64 = 448 px wide)
		 *   - subtitle "no soup for you, only streams" below it
		 *     at 2x scale
		 *   - thin colour bar at the bottom edge */
		framebuffer_fill(0xff001a40u);

		const unsigned big_scale = 8;
		const char    *title     = "kappara";
		uint32_t       title_w   = 7 * 8 * big_scale;
		uint32_t       title_x   = (1024 - title_w) / 2;
		framebuffer_puts(title_x, 120, title,
				 0xffffffffu, 0, big_scale);

		const unsigned sub_scale = 2;
		const char    *sub       = "no soup for you, only streams";
		uint32_t       sub_w     = 29u * 8 * sub_scale;
		uint32_t       sub_x     = (1024 - sub_w) / 2;
		framebuffer_puts(sub_x, 260, sub, 0xffc4c4d4u, 0, sub_scale);

		framebuffer_rect(  0, 760, 256, 8, 0xffe05c5cu);
		framebuffer_rect(256, 760, 256, 8, 0xff5ce05cu);
		framebuffer_rect(512, 760, 256, 8, 0xff5c8ce0u);
		framebuffer_rect(768, 760, 256, 8, 0xffe0c45cu);

		framebuffer_flush();

		/* Position the fbcon cursor below the splash so the
		 * teed kprintf output and any /dev/console writes land
		 * in the lower half of the screen, scrolling under the
		 * "kappara" title rather than over it. */
		fbcon_init_cursor(320);
	}

	kprintf("\n");
	vfs_dump_tree(vfs_root());

	sched_init();
	timer_init(100);
	ipi_init_this_cpu();	/* enable mailbox 0 -> IRQ on core 0 */

	/* Smoke-test the kallsyms table by walking our own frame chain
	 * once -- proves the post-build symbol table is wired up and
	 * the lookup binary-searches correctly.  Trap path uses the
	 * same kernel_backtrace_from() on every unhandled exception. */
	kernel_backtrace();

	/* Console RX feeder: polls PL011 RX FIFO and putnext's received
	 * bytes into /dev/console's read chain.  Spawned before ksh so
	 * the feeder thread is in the ready queue when ksh issues its
	 * first SYS_read. */
	kthread_create("uart_rx", uart_rx_main, NULL);

	/* Spawn the user-mode init process; once scheduled it eret's
	 * into EL0 at user/init.c's _start, opens /dev/console, and
	 * runs the shell entirely in userspace from there on. */
	user_spawn();

	/* SMP foundation: wake cores 1..3 into a per-core "hello +
	 * idle" entry.  They stay at EL2 with MMU off for now; the
	 * actual EL drop + MMU setup + scheduler participation is the
	 * follow-up.  This commit just proves we can light them up. */
	for (unsigned cpu = 1; cpu < 4; cpu++)
		smp_wake_secondary(cpu);

	__asm__ volatile ("msr daifclr, #2");

	/* Main becomes the idle thread: yield forever so the timer/ksh
	 * get all the CPU they want. */
	for (;;) {
		kthread_yield();
		__asm__ volatile ("wfi");
	}
}
