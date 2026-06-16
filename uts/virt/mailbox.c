/*
 * uts/virt/mailbox.c -- VideoCore mailbox stub for QEMU virt
 *
 * The virt machine has no VC4 firmware mailbox.  framebuffer.c on
 * Pi calls this from framebuffer_init() to allocate a frame from the
 * GPU; on virt framebuffer_init() returns -1 unconditionally, so this
 * function is never actually called.  We provide it anyway so any
 * other future caller (e.g. clock/temp queries) gets a clean -1.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/arch/mailbox.h"

int mailbox_property_call(volatile uint32_t *msg, size_t msg_bytes)
{
	(void)msg; (void)msg_bytes;
	return -1;
}
