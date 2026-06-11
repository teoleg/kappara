/*
 * include/kappara/termios.h -- SVR4-flavored termios
 *
 * POSIX terminal-attribute struct used by the ldterm STREAMS module
 * and TCGETA/TCSETA ioctls.  The struct layout matches the historic
 * SVR4 `struct termio` (8-bit flag words, 8-element c_cc); the
 * POSIX `termios` extension to 32-bit flag words + larger c_cc is
 * not modelled yet because nothing here needs it.
 *
 * Phase 2 of the virtual-console roadmap.  See ldterm.c for the
 * line discipline that consumes these flags.
 */
#ifndef KAPPARA_TERMIOS_H
#define KAPPARA_TERMIOS_H

#include <stdint.h>

/* c_iflag -- input modes */
#define IGNBRK		0x0001	/* ignore break condition */
#define BRKINT		0x0002	/* signal interrupt on break */
#define IGNPAR		0x0004	/* ignore chars with parity errors */
#define PARMRK		0x0008	/* mark parity errors */
#define INPCK		0x0010	/* enable input parity check */
#define ISTRIP		0x0020	/* strip 8th bit off chars */
#define INLCR		0x0040	/* map NL to CR on input */
#define IGNCR		0x0080	/* ignore CR */
#define ICRNL		0x0100	/* map CR to NL on input */
#define IUCLC		0x0200	/* map uppercase to lowercase on input */
#define IXON		0x0400	/* enable start/stop output control */
#define IXANY		0x0800	/* any char will restart after stop */
#define IXOFF		0x1000	/* enable start/stop input control */

/* c_oflag -- output modes */
#define OPOST		0x0001	/* postprocess output */
#define OLCUC		0x0002	/* map lowercase to uppercase on output */
#define ONLCR		0x0004	/* map NL to CR-NL on output */
#define OCRNL		0x0008	/* map CR to NL on output */
#define ONOCR		0x0010	/* no CR output at column 0 */
#define ONLRET		0x0020	/* NL performs CR function */

/* c_cflag -- control modes (mostly inert on a software UART/console) */
#define CSIZE		0x0030
#define   CS5		0x0000
#define   CS6		0x0010
#define   CS7		0x0020
#define   CS8		0x0030
#define CSTOPB		0x0040
#define CREAD		0x0080
#define PARENB		0x0100
#define PARODD		0x0200
#define HUPCL		0x0400
#define CLOCAL		0x0800

/* c_lflag -- local (line discipline) modes */
#define ISIG		0x0001	/* enable signals (VINTR, VQUIT, VSUSP) */
#define ICANON		0x0002	/* canonical input (line-buffered + erase/kill) */
#define XCASE		0x0004
#define ECHO		0x0008	/* echo input characters */
#define ECHOE		0x0010	/* visually erase chars on backspace */
#define ECHOK		0x0020	/* echo NL after KILL */
#define ECHONL		0x0040	/* echo NL even if ECHO is off */
#define NOFLSH		0x0080	/* don't flush on signal */
#define TOSTOP		0x0100	/* SIGTTOU on background write */
#define ECHOCTL		0x0200	/* echo control chars as ^X */

/* c_cc indices -- which special-char slot in c_cc[NCCS] holds what.
 * SVR4 termio has 8 entries; the slot numbers match the historic
 * Bell ordering exactly. */
#define NCCS		8

#define VINTR		0	/* INTR  (sig SIGINT)  -- typically ^C */
#define VQUIT		1	/* QUIT  (sig SIGQUIT) -- typically ^\ */
#define VERASE		2	/* ERASE                -- typically ^? or ^H */
#define VKILL		3	/* KILL  (drop line buf) -- typically ^U */
#define VEOF		4	/* EOF                  -- typically ^D */
#define VEOL		5	/* EOL                  -- typically unset */
#define VMIN		4	/* shares slot with VEOF when !ICANON */
#define VTIME		5	/* shares slot with VEOL when !ICANON */

struct termios {
	uint32_t c_iflag;
	uint32_t c_oflag;
	uint32_t c_cflag;
	uint32_t c_lflag;
	uint8_t  c_cc[NCCS];
};

/* Conventional control-char value for "^X" where X is uppercase. */
#define CTRL(x)		((x) & 0x1f)

/* SVR4 termio ioctl numbers.  TCGETA reads the current termios into
 * the user's struct; TCSETA writes it back immediately.  TCSETAW and
 * TCSETAF add output drain + input flush in real Unix; we model both
 * as plain TCSETA for now since the underlying tty is byte-stream
 * fast enough that "drain" is meaningless and "flush" is a no-op
 * without queue depth pressure. */
#define TCGETA		0x5401
#define TCSETA		0x5402
#define TCSETAW		0x5403
#define TCSETAF		0x5404
#define TCFLSH		0x5405

#endif
