/*
 * include/kappara/mailbox.h -- VideoCore mailbox property-tags call
 *
 * Used by drivers that need to talk to the GPU firmware: framebuffer
 * allocation, temperature query, clock setting, etc.
 *
 * Caller owns the message buffer (must be 16-byte aligned, sized to
 * fit the full property-tags packet including end marker).  Returns 0
 * on success, -1 on protocol error.  See arch/aarch64/mailbox.c for
 * the packet layout.
 */
#ifndef KAPPARA_MAILBOX_H
#define KAPPARA_MAILBOX_H

#include <stddef.h>
#include <stdint.h>

int mailbox_property_call(volatile uint32_t *msg, size_t msg_bytes);

#endif
