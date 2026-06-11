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

/* e_machine */
#define EM_AARCH64	0xB7

/* p_type */
#define PT_LOAD		1

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

#endif
