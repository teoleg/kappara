/*
 * include/kappara/elf.h -- ELF64 types needed by the kernel ELF loader
 *
 * Only the fields sys_execve_impl reads are defined.  Full ELF spec is
 * in the System V ABI; the AArch64 supplement specifies e_machine=0xB7.
 */
#ifndef KAPPARA_ELF_H
#define KAPPARA_ELF_H

#include <stdint.h>

/* e_ident indices */
#define EI_MAG0		0
#define EI_MAG1		1
#define EI_MAG2		2
#define EI_MAG3		3
#define EI_CLASS	4	/* 1=32-bit, 2=64-bit */
#define EI_DATA		5	/* 1=LE, 2=BE */

/* e_type */
#define ET_EXEC		2
#define ET_DYN		3	/* PIE / shared object */

/* e_machine */
#define EM_AARCH64	0xB7

/* p_type */
#define PT_LOAD		1
#define PT_DYNAMIC	2	/* points at .dynamic -- runtime fixups */
#define PT_INTERP	3	/* /lib/ld-kappara.so (deferred) */
#define PT_PHDR		6

/* d_tag (PT_DYNAMIC entries) */
#define DT_NULL		0
#define DT_NEEDED	1
#define DT_HASH		4	/* SysV hash table -- nbucket+nchain header
				 * lets us bound dynsym walks */
#define DT_STRTAB	5
#define DT_SYMTAB	6
#define DT_RELA		7	/* offset of Rela table */
#define DT_RELASZ	8	/* size of Rela table in bytes */
#define DT_RELAENT	9	/* size of one Rela entry (should be 24) */
#define DT_STRSZ	10
#define DT_SYMENT	11
#define DT_JMPREL	23	/* PLT Rela table (deferred) */
#define DT_PLTRELSZ	2
#define DT_PLTREL	20

/* AArch64 relocation types (only the ones the loader applies) */
#define R_AARCH64_NONE		0
#define R_AARCH64_RELATIVE	1027	/* *(B + r_offset) = B + r_addend */
#define R_AARCH64_GLOB_DAT	1025	/* deferred (stage 5)            */
#define R_AARCH64_JUMP_SLOT	1026	/* deferred (stage 5)            */

#define ELF64_R_TYPE(info)	((uint32_t)(info))
#define ELF64_R_SYM(info)	((uint32_t)((info) >> 32))

typedef struct {
	uint8_t  e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;	/* program header table offset */
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;	/* offset in file */
	uint64_t p_vaddr;	/* virtual address */
	uint64_t p_paddr;
	uint64_t p_filesz;	/* bytes in file image */
	uint64_t p_memsz;	/* bytes in memory image (>= p_filesz) */
	uint64_t p_align;
} Elf64_Phdr;

/* PT_DYNAMIC entry: each is a (d_tag, d_un) pair.  The address /
 * value disambiguation is by tag (see DT_* table).
 */
typedef struct {
	int64_t  d_tag;
	uint64_t d_un;	/* d_val OR d_ptr -- same slot, tag-disambiguated */
} Elf64_Dyn;

/* Rela: relocation with explicit addend (the only kind aarch64 uses). */
typedef struct {
	uint64_t r_offset;	/* image offset where the fixup lands */
	uint64_t r_info;	/* ELF64_R_TYPE / ELF64_R_SYM packed   */
	int64_t  r_addend;
} Elf64_Rela;

/* Sym: dynamic symbol table entry.  For our cross-DSO resolver we
 * only read st_name (string-table offset), st_value (image VA), and
 * st_shndx (zero == SHN_UNDEF = imported, skip). */
typedef struct {
	uint32_t st_name;
	uint8_t  st_info;
	uint8_t  st_other;
	uint16_t st_shndx;
	uint64_t st_value;
	uint64_t st_size;
} Elf64_Sym;

#endif
