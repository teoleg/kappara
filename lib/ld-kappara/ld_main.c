/*
 * lib/ld-kappara/ld_main.c -- DYNAMIC.md stage 8: user-space linker
 *
 * Replaces the kernel's old (2b) + (2d) reloc walks in sys_execve_impl.
 * The kernel still loads PT_LOADs of the app, libc.so, and ld.so, and
 * builds the auxv -- but it does no relocation work.  This file walks
 * the loaded objects' .rela.dyn / .rela.plt, applies every
 * R_AARCH64_RELATIVE / R_AARCH64_GLOB_DAT / R_AARCH64_JUMP_SLOT, then
 * tail-calls the application's entry point.
 *
 * Freestanding -- no libc.so available (we're loading it).  All
 * helpers are inline; everything ld.so needs to log progress goes
 * through SYS_log directly.
 *
 * Bootstrap requirement: ld.so itself must NOT touch any global it
 * couldn't have computed PC-relative.  We have no relocations applied
 * to ourselves -- the linker.ld pins us at LD_VA and the compiler
 * settles for absolute addressing for string literals (which already
 * sit in the right place).
 */

#include <stdint.h>

#include "kappara/abi/elf.h"

/* Kappara-specific auxv types passed by the kernel.  Standard AT_*
 * are also defined here so we don't drag in <elf.h> at runtime. */
#define AT_NULL                  0
#define AT_PHDR                  3
#define AT_PHNUM                 5
#define AT_PHENT                 4
#define AT_BASE                  7	/* ld.so's own load base */
#define AT_ENTRY                 9
#define AT_KAPPARA_APP_BASE   0x100	/* app's load_base */
#define AT_KAPPARA_LIBC_BASE  0x101	/* libc.so's load_base */

/* SYS_log = 0; matches include/kappara/abi/syscall.h. */
#define SYS_LOG  0

static inline long ld_syscall1(long nr, long a)
{
	register long x0 __asm__("x0") = a;
	register long x8 __asm__("x8") = nr;
	__asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
	return x0;
}

static void ld_log(const char *s) { (void)ld_syscall1(SYS_LOG, (long)(unsigned long)s); }

static int ld_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (unsigned char)*a - (unsigned char)*b;
}

/* Captured layout of an ELF's .dynamic. */
struct ld_dyn {
	uint64_t rela_off,   rela_size;
	uint64_t jmprel_off, jmprel_size;
	uint64_t symtab_off, strtab_off;
	uint64_t strsz;
	uint64_t hash_off;
};

/* Walk PT_DYNAMIC of the object at `load_base`, populating `out`.
 * Object must be already loaded -- we dereference its ELF header
 * at load_base and walk Phdrs through it. */
static int ld_walk_dynamic(uint64_t load_base, struct ld_dyn *out)
{
	const Elf64_Ehdr *eh = (const Elf64_Ehdr *)(uintptr_t)load_base;
	const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(load_base + eh->e_phoff);
	const Elf64_Dyn *dyn = (const Elf64_Dyn *)0;
	uint64_t dyn_size = 0;
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		if (phdrs[i].p_type == PT_DYNAMIC) {
			dyn = (const Elf64_Dyn *)(load_base + phdrs[i].p_vaddr);
			dyn_size = phdrs[i].p_filesz;
			break;
		}
	}
	if (!dyn) return -1;

	out->rela_off = out->rela_size = 0;
	out->jmprel_off = out->jmprel_size = 0;
	out->symtab_off = out->strtab_off = 0;
	out->strsz = out->hash_off = 0;

	unsigned n = (unsigned)(dyn_size / sizeof(Elf64_Dyn));
	for (unsigned i = 0; i < n; i++) {
		switch (dyn[i].d_tag) {
		case DT_NULL:                          return 0;
		case DT_RELA:     out->rela_off     = dyn[i].d_un; break;
		case DT_RELASZ:   out->rela_size    = dyn[i].d_un; break;
		case DT_JMPREL:   out->jmprel_off   = dyn[i].d_un; break;
		case DT_PLTRELSZ: out->jmprel_size  = dyn[i].d_un; break;
		case DT_SYMTAB:   out->symtab_off   = dyn[i].d_un; break;
		case DT_STRTAB:   out->strtab_off   = dyn[i].d_un; break;
		case DT_STRSZ:    out->strsz        = dyn[i].d_un; break;
		case DT_HASH:     out->hash_off     = dyn[i].d_un; break;
		}
	}
	return 0;
}

/* Apply this object's RELATIVE relocs: write (base + r_addend) at
 * (base + r_offset).  Skips other types -- the caller does cross-DSO. */
static void ld_apply_relative(uint64_t base, const struct ld_dyn *dyn)
{
	if (dyn->rela_size == 0) return;
	const Elf64_Rela *relas = (const Elf64_Rela *)(base + dyn->rela_off);
	unsigned n = (unsigned)(dyn->rela_size / sizeof(Elf64_Rela));
	for (unsigned i = 0; i < n; i++) {
		if (ELF64_R_TYPE(relas[i].r_info) != R_AARCH64_RELATIVE)
			continue;
		uint64_t *slot = (uint64_t *)(base + relas[i].r_offset);
		*slot = base + (uint64_t)relas[i].r_addend;
	}
}

/* Number of symbols in a .so via DT_HASH's nchain field. */
static unsigned ld_nsyms(uint64_t base, const struct ld_dyn *dyn)
{
	if (dyn->hash_off == 0) return 0;
	const uint32_t *h = (const uint32_t *)(base + dyn->hash_off);
	return h[1];	/* nchain == nsyms */
}

/* Resolve a symbol name in `provider` (libc.so).  Returns its runtime
 * address (provider_base + sym.st_value) or 0 if absent. */
static uint64_t ld_resolve(uint64_t provider_base,
			   const struct ld_dyn *provider_dyn,
			   const char *name)
{
	const Elf64_Sym *symtab = (const Elf64_Sym *)(provider_base +
						      provider_dyn->symtab_off);
	const char *strtab = (const char *)(provider_base + provider_dyn->strtab_off);
	unsigned nsyms = ld_nsyms(provider_base, provider_dyn);
	if (nsyms == 0 || nsyms > 4096) nsyms = 256;	/* defensive */
	for (unsigned i = 0; i < nsyms; i++) {
		const Elf64_Sym *s = &symtab[i];
		if (s->st_name == 0) continue;
		if (provider_dyn->strsz && s->st_name >= provider_dyn->strsz) continue;
		if (s->st_shndx == 0) continue;	/* SHN_UNDEF */
		if (ld_strcmp(strtab + s->st_name, name) == 0)
			return provider_base + s->st_value;
	}
	return 0;
}

/* Walk a Rela table for cross-DSO entries (GLOB_DAT / JUMP_SLOT) and
 * resolve them against `provider`. */
static int ld_apply_xdso(uint64_t client_base,
			 const Elf64_Rela *relas, unsigned n,
			 uint64_t client_strtab_va,
			 uint64_t client_symtab_va,
			 uint64_t provider_base,
			 const struct ld_dyn *provider_dyn)
{
	const Elf64_Sym  *symtab = (const Elf64_Sym *)client_symtab_va;
	const char       *strtab = (const char *)client_strtab_va;
	for (unsigned i = 0; i < n; i++) {
		uint32_t t = ELF64_R_TYPE(relas[i].r_info);
		if (t != R_AARCH64_GLOB_DAT && t != R_AARCH64_JUMP_SLOT)
			continue;
		uint32_t sym_idx = ELF64_R_SYM(relas[i].r_info);
		const char *name = strtab + symtab[sym_idx].st_name;
		uint64_t resolved = ld_resolve(provider_base, provider_dyn, name);
		if (!resolved) {
			ld_log("ld-kappara: unresolved symbol\n");
			ld_log(name);
			ld_log("\n");
			return -1;
		}
		uint64_t *slot = (uint64_t *)(client_base + relas[i].r_offset);
		*slot = resolved;
	}
	return 0;
}

/* Entry point called from crt0 -- argument: the auxv pointer (after
 * envp NULL on the stack).  Returns the application's entry point;
 * crt0 tail-calls it without disturbing argc/argv.
 *
 * Reads AT_KAPPARA_APP_BASE, AT_KAPPARA_LIBC_BASE, AT_ENTRY.  Walks
 * the dynamic tables of app + libc.so, applies all relocations, then
 * returns AT_ENTRY for the application's _start. */
uint64_t ld_main(const uint64_t *auxv)
{
	uint64_t app_base = 0, libc_base = 0, app_entry = 0;
	for (const uint64_t *p = auxv; p[0] != AT_NULL; p += 2) {
		if (p[0] == AT_KAPPARA_APP_BASE)  app_base  = p[1];
		if (p[0] == AT_KAPPARA_LIBC_BASE) libc_base = p[1];
		if (p[0] == AT_ENTRY)             app_entry = p[1];
	}
	if (!app_entry) {
		ld_log("ld-kappara: no AT_ENTRY in auxv\n");
		return 0;
	}

	/* libc.so first (so cross-DSO lookups have a usable dynsym). */
	struct ld_dyn libc_dyn;
	if (libc_base) {
		if (ld_walk_dynamic(libc_base, &libc_dyn) < 0) {
			ld_log("ld-kappara: libc.so has no PT_DYNAMIC\n");
			return 0;
		}
		ld_apply_relative(libc_base, &libc_dyn);
	}

	/* Application: RELATIVE + cross-DSO. */
	if (app_base) {
		struct ld_dyn app_dyn;
		if (ld_walk_dynamic(app_base, &app_dyn) < 0) {
			ld_log("ld-kappara: app has no PT_DYNAMIC (static-pie OK)\n");
		} else {
			ld_apply_relative(app_base, &app_dyn);

			/* Cross-DSO: needs libc.so loaded + walked. */
			if (libc_base && app_dyn.symtab_off && app_dyn.strtab_off) {
				uint64_t symtab_va = app_base + app_dyn.symtab_off;
				uint64_t strtab_va = app_base + app_dyn.strtab_off;
				if (app_dyn.rela_size > 0) {
					const Elf64_Rela *relas = (const Elf64_Rela *)
						(app_base + app_dyn.rela_off);
					unsigned n = (unsigned)(app_dyn.rela_size /
							        sizeof(Elf64_Rela));
					if (ld_apply_xdso(app_base, relas, n,
							  strtab_va, symtab_va,
							  libc_base, &libc_dyn) < 0)
						return 0;
				}
				if (app_dyn.jmprel_size > 0) {
					const Elf64_Rela *relas = (const Elf64_Rela *)
						(app_base + app_dyn.jmprel_off);
					unsigned n = (unsigned)(app_dyn.jmprel_size /
							        sizeof(Elf64_Rela));
					if (ld_apply_xdso(app_base, relas, n,
							  strtab_va, symtab_va,
							  libc_base, &libc_dyn) < 0)
						return 0;
				}
			}
		}
	}

	return app_entry;
}
