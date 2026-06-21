/*
 * cmd/objdump.c -- minimal ELF header + section/segment dumper.
 *
 * Subset of GNU objdump.  Supports:
 *   objdump -h <file>     section headers
 *   objdump -p <file>     program headers
 *   objdump -x <file>     all of the above
 *   objdump <file>        same as -x
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "kappara/abi/elf.h"

static unsigned char buf[64 * 1024];

/* Section header (subset).  Mirrored from the System V ABI. */
typedef struct {
	uint32_t sh_name;
	uint32_t sh_type;
	uint64_t sh_flags;
	uint64_t sh_addr;
	uint64_t sh_offset;
	uint64_t sh_size;
	uint32_t sh_link;
	uint32_t sh_info;
	uint64_t sh_addralign;
	uint64_t sh_entsize;
} Elf64_Shdr;

static const char *p_type_name(uint32_t t)
{
	switch (t) {
	case 1: return "LOAD";
	case 2: return "DYNAMIC";
	case 3: return "INTERP";
	case 4: return "NOTE";
	case 6: return "PHDR";
	case 7: return "TLS";
	case 0x6474e550: return "GNU_EH_FRAME";
	case 0x6474e551: return "GNU_STACK";
	case 0x6474e552: return "GNU_RELRO";
	default: return "?";
	}
}

static void print_phdrs(Elf64_Ehdr *eh)
{
	printf("Program headers:\n");
	printf("  %-12s %-16s %-16s %-8s %s\n",
	       "Type", "VirtAddr", "FileSize", "Align", "Flags");
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		Elf64_Phdr *ph = (Elf64_Phdr *)(buf + eh->e_phoff +
				 (uint64_t)i * eh->e_phentsize);
		char flags[4] = "---";
		if (ph->p_flags & 4) flags[0] = 'R';
		if (ph->p_flags & 2) flags[1] = 'W';
		if (ph->p_flags & 1) flags[2] = 'X';
		printf("  %-12s %016lx %016lx %8lx %s\n",
		       p_type_name(ph->p_type),
		       (unsigned long)ph->p_vaddr,
		       (unsigned long)ph->p_filesz,
		       (unsigned long)ph->p_align,
		       flags);
	}
}

static void print_shdrs(Elf64_Ehdr *eh)
{
	if (eh->e_shoff == 0 || eh->e_shnum == 0) {
		printf("Section headers: (stripped)\n");
		return;
	}
	Elf64_Shdr *shdrs = (Elf64_Shdr *)(buf + eh->e_shoff);
	const char *shstrtab = NULL;
	if (eh->e_shstrndx < eh->e_shnum)
		shstrtab = (const char *)(buf + shdrs[eh->e_shstrndx].sh_offset);

	printf("Section headers:\n");
	printf("  %-3s %-20s %-16s %-16s %s\n",
	       "Nr", "Name", "Addr", "Offset", "Size");
	for (unsigned i = 0; i < eh->e_shnum; i++) {
		const char *name = (shstrtab && shdrs[i].sh_name) ?
			(shstrtab + shdrs[i].sh_name) : "(unnamed)";
		printf("  %3u %-20s %016lx %016lx %lx\n",
		       i, name,
		       (unsigned long)shdrs[i].sh_addr,
		       (unsigned long)shdrs[i].sh_offset,
		       (unsigned long)shdrs[i].sh_size);
	}
}

static void print_header(Elf64_Ehdr *eh)
{
	const char *type;
	switch (eh->e_type) {
	case 1: type = "REL"; break;
	case 2: type = "EXEC"; break;
	case 3: type = "DYN"; break;
	case 4: type = "CORE"; break;
	default: type = "?"; break;
	}
	printf("ELF header:\n");
	printf("  Class:   ELF64\n");
	printf("  Type:    %s\n", type);
	printf("  Machine: 0x%x%s\n", eh->e_machine,
	       eh->e_machine == 0xB7 ? " (AArch64)" : "");
	printf("  Entry:   0x%lx\n", (unsigned long)eh->e_entry);
	printf("  Phdrs:   %u entries @ offset 0x%lx\n",
	       eh->e_phnum, (unsigned long)eh->e_phoff);
	printf("  Shdrs:   %u entries @ offset 0x%lx\n",
	       eh->e_shnum, (unsigned long)eh->e_shoff);
}

int main(int argc, char **argv)
{
	int want_p = 0, want_h = 0;
	const char *path = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-p") == 0)        want_p = 1;
		else if (strcmp(argv[i], "-h") == 0)   want_h = 1;
		else if (strcmp(argv[i], "-x") == 0)   { want_p = want_h = 1; }
		else if (argv[i][0] != '-')            path = argv[i];
	}
	if (!path) {
		puts("usage: objdump [-h|-p|-x] <elf-file>");
		return 1;
	}
	if (!want_p && !want_h) { want_p = want_h = 1; }	/* default -x */

	int fd = open(path, 0);
	if (fd < 0) { printf("objdump: cannot open '%s'\n", path); return 1; }

	ssize_t total = 0;
	for (;;) {
		ssize_t n = read(fd, buf + total, sizeof(buf) - (size_t)total);
		if (n <= 0) break;
		total += n;
	}
	close(fd);
	if ((size_t)total < sizeof(Elf64_Ehdr)) {
		printf("objdump: file too small\n"); return 1;
	}

	Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E') {
		printf("objdump: not an ELF\n"); return 1;
	}

	print_header(eh);
	if (want_p) { printf("\n"); print_phdrs(eh); }
	if (want_h) { printf("\n"); print_shdrs(eh); }
	return 0;
}
