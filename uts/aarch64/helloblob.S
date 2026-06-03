/*
 * arch/aarch64/helloblob.S -- embed hello.elf into the kernel image
 *
 * Parallel to userblob.S (which carries init.bin).  hello_blob_start /
 * hello_blob_end bracket the raw ELF bytes; kernel/user.c registers
 * them as a read-only blob file at /bin/hello so sys_execve can load it.
 */

.section ".rodata.hello_blob","a",%progbits

.global hello_blob_start
.global hello_blob_end
.balign 8
hello_blob_start:
.incbin "build/user/hello.elf"
hello_blob_end:
