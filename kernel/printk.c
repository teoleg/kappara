/*
 * kernel/printk.c -- freestanding kernel printf
 * =============================================
 *
 * What this file is
 * -----------------
 * A small printf-flavoured formatter that talks straight to the UART
 * via uart_putc().  Arch-independent: it neither knows nor cares
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
 *     emit_str  loops uart_putc; '\n' is translated to "\r\n" so a
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

#include "kappara/printk.h"
#include "kappara/uart.h"

static void emit_str(const char *s)
{
	if (!s) s = "(null)";
	while (*s) {
		if (*s == '\n')
			uart_putc('\r');
		uart_putc(*s++);
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
			uart_putc(buf[i]);
	} else {
		int written = i;
		while (i--)
			uart_putc(buf[i]);
		while (written++ < width)
			uart_putc(' ');
	}
}

static void emit_int(int64_t v, int width, char pad, int left)
{
	if (v < 0) {
		uart_putc('-');
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
				uart_putc('\r');
			uart_putc(*fmt++);
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
			uart_putc((char)va_arg(ap, int));
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
			uart_putc('%');
			break;
		case '\0':
			return;
		default:
			uart_putc('%');
			uart_putc(*fmt);
			break;
		}
		fmt++;
	}
}

void kprintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vkprintf(fmt, ap);
	va_end(ap);
}

void kpanic(const char *msg)
{
	kprintf("panic: %s\n", msg);
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
