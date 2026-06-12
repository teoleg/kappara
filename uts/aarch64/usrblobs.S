/*
 * arch/aarch64/usrblobs.S -- /usr/bin/ program blobs
 *
 * Each cmd/ ELF is incbin'd into a named .rodata section.
 * kernel/user.c registers them in VFS at exec_space_init time.
 */

	.section ".rodata.ps_blob","a",%progbits
	.balign 8
	.global ps_blob_start
	.global ps_blob_end
ps_blob_start:
	.incbin "build/cmd/ps.elf"
ps_blob_end:

	.section ".rodata.sigtest_blob","a",%progbits
	.balign 8
	.global sigtest_blob_start
	.global sigtest_blob_end
sigtest_blob_start:
	.incbin "build/cmd/sigtest.elf"
sigtest_blob_end:

	.section ".rodata.masktest_blob","a",%progbits
	.balign 8
	.global masktest_blob_start
	.global masktest_blob_end
masktest_blob_start:
	.incbin "build/cmd/masktest.elf"
masktest_blob_end:

	.section ".rodata.waittest_blob","a",%progbits
	.balign 8
	.global waittest_blob_start
	.global waittest_blob_end
waittest_blob_start:
	.incbin "build/cmd/waittest.elf"
waittest_blob_end:

	.section ".rodata.segvtest_blob","a",%progbits
	.balign 8
	.global segvtest_blob_start
	.global segvtest_blob_end
segvtest_blob_start:
	.incbin "build/cmd/segvtest.elf"
segvtest_blob_end:

	.section ".rodata.pipe_blob","a",%progbits
	.balign 8
	.global pipe_blob_start
	.global pipe_blob_end
pipe_blob_start:
	.incbin "build/cmd/pipe.elf"
pipe_blob_end:

	.section ".rodata.pipework_blob","a",%progbits
	.balign 8
	.global pipework_blob_start
	.global pipework_blob_end
pipework_blob_start:
	.incbin "build/cmd/pipework.elf"
pipework_blob_end:

	.section ".rodata.malloctest_blob","a",%progbits
	.balign 8
	.global malloctest_blob_start
	.global malloctest_blob_end
malloctest_blob_start:
	.incbin "build/cmd/malloctest.elf"
malloctest_blob_end:

	.section ".rodata.forktest_blob","a",%progbits
	.balign 8
	.global forktest_blob_start
	.global forktest_blob_end
forktest_blob_start:
	.incbin "build/cmd/forktest.elf"
forktest_blob_end:

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

	.section ".rodata.udptest_blob","a",%progbits
	.balign 8
	.global udptest_blob_start
	.global udptest_blob_end
udptest_blob_start:
	.incbin "build/cmd/udptest.elf"
udptest_blob_end:

	.section ".rodata.pktfilttest_blob","a",%progbits
	.balign 8
	.global pktfilttest_blob_start
	.global pktfilttest_blob_end
pktfilttest_blob_start:
	.incbin "build/cmd/pktfilttest.elf"
pktfilttest_blob_end:

	.section ".rodata.tcptest_blob","a",%progbits
	.balign 8
	.global tcptest_blob_start
	.global tcptest_blob_end
tcptest_blob_start:
	.incbin "build/cmd/tcptest.elf"
tcptest_blob_end:
