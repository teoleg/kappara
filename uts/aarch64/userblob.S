/*
 * arch/aarch64/userblob.S -- embed the linked user binary
 *
 * .incbin sucks the raw bytes of build/user/init.bin into the kernel
 * image, bracketed by two global symbols the C side can use:
 *
 *   user_blob_start
 *   user_blob_end
 *
 * kernel/user.c copies (user_blob_end - user_blob_start) bytes into
 * the 2 MB user_storage region at user_init time.  The bytes start
 * with _start (linker.ld forces .text._start first), so the entry
 * point is just USER_VA + 0.
 */

.section ".rodata.user_blob","a",%progbits

.global user_blob_start
.global user_blob_end

user_blob_start:
.incbin "build/user/init.bin"
user_blob_end:
