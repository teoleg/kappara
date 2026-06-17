/*
 * arch/aarch64/usrblobs.S -- /usr/bin/ program blobs
 *
 * Each cmd/ ELF is incbin'd into a named .rodata section.
 * kernel/user.c registers them in VFS at exec_space_init time.
 *
 * Going forward, prefer adding new selftests to cmd/test.c rather
 * than creating new top-level ELFs -- KFS_DIRENTS caps /usr/bin at
 * 14 files, and each new ELF burns kernel image space via incbin.
 */

	.section ".rodata.ps_blob","a",%progbits
	.balign 8
	.global ps_blob_start
	.global ps_blob_end
ps_blob_start:
	.incbin "build/cmd/ps.elf"
ps_blob_end:

	.section ".rodata.ping_blob","a",%progbits
	.balign 8
	.global ping_blob_start
	.global ping_blob_end
ping_blob_start:
	.incbin "build/cmd/ping.elf"
ping_blob_end:

	.section ".rodata.ifconfig_blob","a",%progbits
	.balign 8
	.global ifconfig_blob_start
	.global ifconfig_blob_end
ifconfig_blob_start:
	.incbin "build/cmd/ifconfig.elf"
ifconfig_blob_end:

	.section ".rodata.netstat_blob","a",%progbits
	.balign 8
	.global netstat_blob_start
	.global netstat_blob_end
netstat_blob_start:
	.incbin "build/cmd/netstat.elf"
netstat_blob_end:

	.section ".rodata.test_blob","a",%progbits
	.balign 8
	.global test_blob_start
	.global test_blob_end
test_blob_start:
	.incbin "build/cmd/test.elf"
test_blob_end:

	.section ".rodata.tcpconnect_blob","a",%progbits
	.balign 8
	.global tcpconnect_blob_start
	.global tcpconnect_blob_end
tcpconnect_blob_start:
	.incbin "build/cmd/tcpconnect.elf"
tcpconnect_blob_end:
