/*
 * uts/virt/slip-stubs.c -- SLIP no-ops for QEMU virt
 *
 * The virt machine has no mini-UART so slip0 doesn't exist.  Provide
 * the four symbols stream_head.c and procfs.c reference so the link
 * stays clean -- slip_init / slip_module_init are no-ops, the stats
 * accessors return zeros, and the /proc/slip walker quietly reports
 * "(slip not initialised)".  Phase 3 replaces this whole file with
 * the virtio-net driver; until then, virt has no SLIP path and no
 * UART-backed networking.
 */

#include <stdint.h>

#include "kappara/net/slip.h"

void slip_init       (void) { }
void slip_module_init(void) { }

uint32_t slip_rx_byte_count(void)
{
	return 0;
}

int slip_get_stats(struct slip_state_view *out)
{
	(void)out;
	return -1;	/* "not initialised" -- procfs prints a placeholder */
}
