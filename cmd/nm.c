/*
 * cmd/nm.c -- minimal ELF symbol-table dumper.
 *
 * Reads a static or dynamic ELF and prints one line per dynsym entry:
 *   value type bind name
 * where:
 *   value = st_value as 16-char hex (relative offset for ET_DYN)
 *   type  = STT_FUNC -> T, STT_OBJECT -> D, STT_NOTYPE -> N, other -> ?
 *   bind  = STB_GLOBAL -> uppercase, STB_LOCAL -> lowercase
 *
 * Doesn't cover .symtab (stripped libraries hide it); .dynsym is enough
 * for inspecting our /lib shared objects.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kappara/abi/elf.h"

static unsigned char buf[64 * 1024];

static void dump(const char *path)
{
	int fd = open(path, 0);
	if (fd < 0) { printf("nm: cannot open '%s'\n", path); return; }

	ssize_t total = 0;
	for (;;) {
		ssize_t n = read(fd, buf + total, sizeof(buf) - (size_t)total);
		if (n <= 0) break;
		total += n;
	}
	close(fd);

	if ((size_t)total < sizeof(Elf64_Ehdr)) {
		printf("nm: '%s' too small\n", path);
		return;
	}

	Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
		printf("nm: '%s' not an ELF\n", path);
		return;
	}

	/* Walk PT_DYNAMIC for DT_SYMTAB/DT_STRTAB/DT_HASH. */
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
		printf("nm: '%s' has no PT_DYNAMIC\n", path);
		return;
	}

	uint64_t symtab_va = 0, strtab_va = 0, hash_va = 0;
	Elf64_Dyn *dyn = (Elf64_Dyn *)(buf + dyn_off);
	unsigned dn = (unsigned)(dyn_size / sizeof(Elf64_Dyn));
	for (unsigned i = 0; i < dn; i++) {
		if (dyn[i].d_tag == DT_NULL) break;
		if (dyn[i].d_tag == DT_SYMTAB) symtab_va = dyn[i].d_un;
		if (dyn[i].d_tag == DT_STRTAB) strtab_va = dyn[i].d_un;
		if (dyn[i].d_tag == DT_HASH)   hash_va   = dyn[i].d_un;
	}

	/* Translate image VA -> file offset by walking PT_LOADs. */
	uint64_t symtab_off = 0, strtab_off = 0, hash_off = 0;
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		Elf64_Phdr *ph = (Elf64_Phdr *)(buf + eh->e_phoff +
				 (uint64_t)i * eh->e_phentsize);
		if (ph->p_type != PT_LOAD) continue;
		uint64_t lo = ph->p_vaddr, hi = ph->p_vaddr + ph->p_filesz;
		if (symtab_va && symtab_va >= lo && symtab_va < hi)
			symtab_off = ph->p_offset + (symtab_va - lo);
		if (strtab_va && strtab_va >= lo && strtab_va < hi)
			strtab_off = ph->p_offset + (strtab_va - lo);
		if (hash_va   && hash_va   >= lo && hash_va   < hi)
			hash_off   = ph->p_offset + (hash_va   - lo);
	}

	if (!symtab_off || !strtab_off) {
		printf("nm: '%s' missing dynsym/dynstr\n", path);
		return;
	}

	unsigned nsyms = 0;
	if (hash_off) {
		uint32_t *h = (uint32_t *)(buf + hash_off);
		nsyms = h[1];	/* nchain */
	} else {
		nsyms = 64;	/* defensive cap when no DT_HASH */
	}

	Elf64_Sym *symtab = (Elf64_Sym *)(buf + symtab_off);
	const char *strtab = (const char *)(buf + strtab_off);

	for (unsigned i = 0; i < nsyms; i++) {
		Elf64_Sym *s = &symtab[i];
		if (s->st_name == 0) continue;
		unsigned type = s->st_info & 0xf;
		unsigned bind = s->st_info >> 4;
		char t = '?';
		if (s->st_shndx == 0) t = 'U';		/* SHN_UNDEF */
		else if (type == 2) t = 'T';		/* STT_FUNC */
		else if (type == 1) t = 'D';		/* STT_OBJECT */
		else if (type == 0) t = 'N';		/* STT_NOTYPE */
		if (bind == 0 && t != '?' && t != 'U')	/* STB_LOCAL */
			t = (char)(t + 'a' - 'A');
		printf("%016lx %c %s\n",
		       (unsigned long)s->st_value, t,
		       strtab + s->st_name);
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		puts("usage: nm <elf-file> [...]");
		return 1;
	}
	for (int i = 1; i < argc; i++) {
		if (argc > 2) printf("\n%s:\n", argv[i]);
		dump(argv[i]);
	}
	return 0;
}
