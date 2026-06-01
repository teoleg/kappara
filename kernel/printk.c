/*
 * kernel/printk.c -- freestanding kernel printf
 * =============================================
 *
 * What this file is
 * -----------------
 * A small printf-flavoured formatter that talks straight to the UART
 * via kputc().  Arch-independent: it neither knows nor cares
 * which board it's on, and the same source compiles for both
 * AArch64 and ARMv7.
 *
 * Why not use libc?
 * -----------------
 * We compile -ffreestanding -nostdlib.  There is no malloc, no
 * vsnprintf, no FILE.  The only standard-library headers we touch
 * (<stdint.h>, <stdarg.h>, <stddef.h>) are part of the compiler
 * itself, not libc, so they work in freestanding mode.  Everything
 * else we write by hand.
 *
 * Supported format directives
 * ---------------------------
 *
 *     %d / %i   signed decimal      (int or long with %ld)
 *     %u        unsigned decimal    (int or long with %lu)
 *     %x        lowercase hex       (int or long with %lx)
 *     %X        uppercase hex
 *     %s        NUL-terminated string (NULL -> "(null)")
 *     %c        single char (passed as int after default promotion)
 *     %p        pointer, formatted as 0x%016lx
 *     %%        literal '%'
 *
 *     Flags / width:  '-' (left-align)
 *                     '0' (zero-pad numeric)
 *                     width digits
 *                     'l' (modifier, promote int -> long)
 *
 * Implementation sketch
 * ---------------------
 *
 *     vkprintf  scans fmt, calls emit_str / emit_int / emit_uint
 *               for each directive
 *     emit_uint accumulates digits into a small reverse-order buffer
 *               on the stack (max 64 hex digits is enough for any
 *               uint64_t), then either prepends padding and dumps,
 *               or dumps then appends padding (left-align case)
 *     emit_str  loops kputc; '\n' is translated to "\r\n" so a
 *               raw serial terminal stays in column 0
 *
 * kpanic
 * ------
 * Final stop for unrecoverable errors.  Prints a "panic: ..." line
 * via the same formatter, then wfe-loops forever.
 *
 * raise
 * -----
 * libgcc's 32-bit division-by-zero helpers reference raise() on the
 * Linux-EABI build of libgcc that ships with the Debian cross.  We
 * stub it to a kpanic so the linker is happy and we still get a
 * useful message if the kernel actually does divide by zero.
 */

#include <stdarg.h>
#include <stdint.h>

#include "kappara/fbcon.h"
#include "kappara/klog.h"
#include "kappara/printk.h"
#include "kappara/spinlock.h"
#include "kappara/uart.h"

/*
 * One kprintf at a time across all CPUs.  Without this, every core
 * writes through uart_putc concurrently and the UART output ends up
 * with bytes from different lines interleaved -- you cannot read
 * boot messages from secondary cores.
 *
 * Locking granularity is whole-kprintf, not per-character: it costs
 * the same lock acquire either way, and a complete line shows up
 * intact instead of being smeared across all callers' output.
 *
 * IRQ-save: kprintf is called from any context including the timer
 * IRQ (sched_tick uses kprintf for diagnostic prints).  Without
 * masking, an IRQ that preempts a holder on the same CPU and tries
 * to print itself would deadlock spinning on its own lock.  Panic
 * path deliberately bypasses the lock (a stuck CPU must not silence
 * the panic message).
 */
static spinlock_t kprintf_lock = SPINLOCK_INIT;

/*
 * Every byte that kprintf would emit goes three places at once:
 *   uart_putc  -- serial console (always)
 *   klog_putc  -- in-RAM boot log ring (always)
 *   fbcon_putc_tee -- framebuffer console (no-op until the GPU
 *                     framebuffer is up; live for the rest of boot
 *                     so the kernel log scrolls down the HDMI screen
 *                     alongside the splash artwork)
 */
static void kputc(char c)
{
	uart_putc(c);
	klog_putc(c);
	fbcon_putc_tee(c);
}

static void emit_str(const char *s)
{
	if (!s) s = "(null)";
	while (*s) {
		if (*s == '\n')
			kputc('\r');
		kputc(*s++);
	}
}

static void emit_uint(uint64_t v, unsigned base, int width, char pad,
		      int upper, int left)
{
	char buf[65];
	int i = 0;
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

	if (v == 0)
		buf[i++] = '0';
	while (v) {
		buf[i++] = digits[v % base];
		v /= base;
	}

	if (!left) {
		while (i < width)
			buf[i++] = pad;
		while (i--)
			kputc(buf[i]);
	} else {
		int written = i;
		while (i--)
			kputc(buf[i]);
		while (written++ < width)
			kputc(' ');
	}
}

static void emit_int(int64_t v, int width, char pad, int left)
{
	if (v < 0) {
		kputc('-');
		v = -v;
		if (width > 0)
			width--;
	}
	emit_uint((uint64_t)v, 10, width, pad, 0, left);
}

void vkprintf(const char *fmt, va_list ap)
{
	while (*fmt) {
		if (*fmt != '%') {
			if (*fmt == '\n')
				kputc('\r');
			kputc(*fmt++);
			continue;
		}
		fmt++;

		char pad = ' ';
		int width = 0;
		int longflag = 0;
		int left = 0;

		if (*fmt == '-') { left = 1; fmt++; }
		if (*fmt == '0') { pad = '0'; fmt++; }
		while (*fmt >= '0' && *fmt <= '9') {
			width = width * 10 + (*fmt - '0');
			fmt++;
		}
		if (*fmt == 'l') { longflag = 1; fmt++; }
		if (*fmt == 'l') { fmt++; }

		switch (*fmt) {
		case 's':
			emit_str(va_arg(ap, const char *));
			break;
		case 'c':
			kputc((char)va_arg(ap, int));
			break;
		case 'd':
		case 'i':
			if (longflag) emit_int(va_arg(ap, long), width, pad, left);
			else          emit_int(va_arg(ap, int),  width, pad, left);
			break;
		case 'u':
			if (longflag) emit_uint(va_arg(ap, unsigned long), 10, width, pad, 0, left);
			else          emit_uint(va_arg(ap, unsigned int),  10, width, pad, 0, left);
			break;
		case 'x':
			if (longflag) emit_uint(va_arg(ap, unsigned long), 16, width, pad, 0, left);
			else          emit_uint(va_arg(ap, unsigned int),  16, width, pad, 0, left);
			break;
		case 'X':
			if (longflag) emit_uint(va_arg(ap, unsigned long), 16, width, pad, 1, left);
			else          emit_uint(va_arg(ap, unsigned int),  16, width, pad, 1, left);
			break;
		case 'p':
			emit_str("0x");
			emit_uint((uint64_t)(uintptr_t)va_arg(ap, void *), 16, 16, '0', 0, 0);
			break;
		case '%':
			kputc('%');
			break;
		case '\0':
			return;
		default:
			kputc('%');
			kputc(*fmt);
			break;
		}
		fmt++;
	}
}

void kprintf(const char *fmt, ...)
{
	va_list ap;
	unsigned long flags = spin_lock_irq_save(&kprintf_lock);
	va_start(ap, fmt);
	vkprintf(fmt, ap);
	va_end(ap);
	/* One batched fb flush per kprintf call -- amortises the
	 * 3 MB dc cvac sweep across many characters. */
	fbcon_tee_flush();
	spin_unlock_irq_restore(&kprintf_lock, flags);
}

void kpanic(const char *msg)
{
	/*
	 * Panic bypasses kprintf's lock: another CPU may have crashed
	 * while holding it, and the whole point of a panic message is
	 * that it gets out no matter what.  Best-effort direct uart.
	 */
	uart_puts("\npanic: ");
	uart_puts(msg);
	uart_puts("\n");
	for (;;)
		__asm__ volatile ("wfe");
}

/* libgcc helpers (notably __aeabi_ldiv0) call raise() on math errors. */
int raise(int signum);
int raise(int signum)
{
	(void)signum;
	kpanic("libgcc raise() called");
}
