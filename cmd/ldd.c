/*
 * cmd/ldd.c -- minimal "ldd" -- list dynamic shared library
 * dependencies (DT_NEEDED entries) of an ELF.
 *
 * No actual resolution: we don't search libraries.  Just list each
 * NEEDED entry as the binary advertises it.  Kappara's loader treats
 * libc.so as a hard-wired dependency, so most binaries print:
 *
 *     libc.so
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "kappara/abi/elf.h"

static unsigned char buf[64 * 1024];

static void dump(const char *path)
{
	int fd = open(path, 0);
	if (fd < 0) { printf("ldd: cannot open '%s'\n", path); return; }

	ssize_t total = 0;
	for (;;) {
		ssize_t n = read(fd, buf + total, sizeof(buf) - (size_t)total);
		if (n <= 0) break;
		total += n;
	}
	close(fd);

	if ((size_t)total < sizeof(Elf64_Ehdr)) {
		printf("ldd: '%s' too small\n", path);
		return;
	}

	Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E') {
		printf("ldd: '%s' not an ELF\n", path); return;
	}

	uint64_t dyn_off = 0, dyn_size = 0;
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		Elf64_Phdr *ph = (Elf64_Phdr *)(buf + eh->e_phoff +
				 (uint64_t)i * eh->e_phentsize);
		if (ph->p_type == PT_DYNAMIC) {
			dyn_off  = ph->p_offset;
			dyn_size = ph->p_filesz;
			break;
		}
	}
	if (!dyn_off) {
		printf("\t(no dependencies -- static binary)\n");
		return;
	}

	uint64_t strtab_va = 0;
	Elf64_Dyn *dyn = (Elf64_Dyn *)(buf + dyn_off);
	unsigned dn = (unsigned)(dyn_size / sizeof(Elf64_Dyn));
	for (unsigned i = 0; i < dn; i++) {
		if (dyn[i].d_tag == DT_NULL) break;
		if (dyn[i].d_tag == DT_STRTAB) strtab_va = dyn[i].d_un;
	}

	uint64_t strtab_off = 0;
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		Elf64_Phdr *ph = (Elf64_Phdr *)(buf + eh->e_phoff +
				 (uint64_t)i * eh->e_phentsize);
		if (ph->p_type != PT_LOAD) continue;
		uint64_t lo = ph->p_vaddr, hi = ph->p_vaddr + ph->p_filesz;
		if (strtab_va && strtab_va >= lo && strtab_va < hi)
			strtab_off = ph->p_offset + (strtab_va - lo);
	}
	if (!strtab_off) {
		printf("\t(no strtab)\n"); return;
	}

	const char *strtab = (const char *)(buf + strtab_off);
	int any = 0;
	for (unsigned i = 0; i < dn; i++) {
		if (dyn[i].d_tag == DT_NULL) break;
		if (dyn[i].d_tag == DT_NEEDED) {
			printf("\t%s\n", strtab + dyn[i].d_un);
			any = 1;
		}
	}
	if (!any) printf("\t(no dependencies)\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		puts("usage: ldd <elf-file> [...]");
		return 1;
	}
	for (int i = 1; i < argc; i++) {
		if (argc > 2) printf("\n%s:\n", argv[i]);
		dump(argv[i]);
	}
	return 0;
}
