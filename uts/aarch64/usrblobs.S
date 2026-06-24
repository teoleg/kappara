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

	.section ".rodata.ftpd_blob","a",%progbits
	.balign 8
	.global ftpd_blob_start
	.global ftpd_blob_end
ftpd_blob_start:
	.incbin "build/cmd/ftpd.elf"
ftpd_blob_end:

	.section ".rodata.ls_blob","a",%progbits
	.balign 8
	.global ls_blob_start
	.global ls_blob_end
ls_blob_start:
	.incbin "build/cmd/ls.elf"
ls_blob_end:

	.section ".rodata.cat_blob","a",%progbits
	.balign 8
	.global cat_blob_start
	.global cat_blob_end
cat_blob_start:
	.incbin "build/cmd/cat.elf"
cat_blob_end:

	.section ".rodata.cp_blob","a",%progbits
	.balign 8
	.global cp_blob_start
	.global cp_blob_end
cp_blob_start:
	.incbin "build/cmd/cp.elf"
cp_blob_end:

	.section ".rodata.mv_blob","a",%progbits
	.balign 8
	.global mv_blob_start
	.global mv_blob_end
mv_blob_start:
	.incbin "build/cmd/mv.elf"
mv_blob_end:

	.section ".rodata.rm_blob","a",%progbits
	.balign 8
	.global rm_blob_start
	.global rm_blob_end
rm_blob_start:
	.incbin "build/cmd/rm.elf"
rm_blob_end:

	.section ".rodata.ll_blob","a",%progbits
	.balign 8
	.global ll_blob_start
	.global ll_blob_end
ll_blob_start:
	.incbin "build/cmd/ll.elf"
ll_blob_end:

	.section ".rodata.head_blob","a",%progbits
	.balign 8
	.global head_blob_start
	.global head_blob_end
head_blob_start:
	.incbin "build/cmd/head.elf"
head_blob_end:

	.section ".rodata.tail_blob","a",%progbits
	.balign 8
	.global tail_blob_start
	.global tail_blob_end
tail_blob_start:
	.incbin "build/cmd/tail.elf"
tail_blob_end:

	.section ".rodata.wc_blob","a",%progbits
	.balign 8
	.global wc_blob_start
	.global wc_blob_end
wc_blob_start:
	.incbin "build/cmd/wc.elf"
wc_blob_end:

	.section ".rodata.grep_blob","a",%progbits
	.balign 8
	.global grep_blob_start
	.global grep_blob_end
grep_blob_start:
	.incbin "build/cmd/grep.elf"
grep_blob_end:

	.section ".rodata.echo_blob","a",%progbits
	.balign 8
	.global echo_blob_start
	.global echo_blob_end
echo_blob_start:
	.incbin "build/cmd/echo.elf"
echo_blob_end:

	.section ".rodata.uptime_blob","a",%progbits
	.balign 8
	.global uptime_blob_start
	.global uptime_blob_end
uptime_blob_start:
	.incbin "build/cmd/uptime.elf"
uptime_blob_end:

	.section ".rodata.nm_blob","a",%progbits
	.balign 8
	.global nm_blob_start
	.global nm_blob_end
nm_blob_start:
	.incbin "build/cmd/nm.elf"
nm_blob_end:

	.section ".rodata.ldd_blob","a",%progbits
	.balign 8
	.global ldd_blob_start
	.global ldd_blob_end
ldd_blob_start:
	.incbin "build/cmd/ldd.elf"
ldd_blob_end:

	.section ".rodata.objdump_blob","a",%progbits
	.balign 8
	.global objdump_blob_start
	.global objdump_blob_end
objdump_blob_start:
	.incbin "build/cmd/objdump.elf"
objdump_blob_end:

	.section ".rodata.host_blob","a",%progbits
	.balign 8
	.global host_blob_start
	.global host_blob_end
host_blob_start:
	.incbin "build/cmd/host.elf"
host_blob_end:

/* ld-kappara.so -- stage 5 dynamic linker.  Not a /usr/bin entry;
 * the kernel reads it directly via ld_kappara_blob_{start,end}
 * during exec to load it into the user vm_map at LD_VA. */
	.section ".rodata.ld_kappara_blob","a",%progbits
	.balign 8
	.global ld_kappara_blob_start
	.global ld_kappara_blob_end
ld_kappara_blob_start:
	.incbin "build/ld-kappara/ld-kappara.so"
ld_kappara_blob_end:

/* libc.so -- stage 6 shared C library.  Same blob shape as
 * ld-kappara.so; kernel reads via libc_so_blob_{start,end} during
 * exec, loads at LIBC_VA, applies its RELATIVE relocs, and resolves
 * cmd binaries' GLOB_DAT / JUMP_SLOT entries against its dynsym. */
	.section ".rodata.libc_so_blob","a",%progbits
	.balign 8
	.global libc_so_blob_start
	.global libc_so_blob_end
libc_so_blob_start:
	.incbin "build/cmd/libc/libc.so"
libc_so_blob_end:

/* libdltest.so -- stage 7 dlopen smoke test target.  Tiny self-
 * contained ET_DYN with no DT_NEEDED.  Registered at /lib/libdltest.so;
 * the dlopen syscall reads it via vfs and loads at DLOPEN_VA. */
	.section ".rodata.libdltest_blob","a",%progbits
	.balign 8
	.global libdltest_blob_start
	.global libdltest_blob_end
libdltest_blob_start:
	.incbin "build/dltest/libdltest.so"
libdltest_blob_end:
