/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ELF64 wire fields used by the fork's bounded firmware relocation loader. */
#ifndef VMM_PLATFORM_ELF_H
#define VMM_PLATFORM_ELF_H
#include <stdint.h>
typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf64_Ehdr;
typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} Elf64_Phdr;
typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} Elf64_Shdr;
typedef struct {
    uint32_t st_name;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} Elf64_Sym;
typedef struct {
    uint64_t offset, info, addend;
} Elf64_Rela;
#define ELFMAG "\177ELF"
#define SELFMAG 4
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ET_EXEC 2
#define EM_IA_64 50
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_DYNAMIC 6
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define ELF64_R_TYPE(i) ((uint32_t)(i))
#define ELF64_R_SYM(i) ((i) >> 32)
#define R_IA64_NONE 0
#define R_IA64_IMM64 0x23
#define R_IA64_DIR64LSB 0x27
#define R_IA64_GPREL22 0x2a
#define R_IA64_GPREL64I 0x2b
#define R_IA64_LTOFF22 0x32
#define R_IA64_FPTR64LSB 0x47
#define R_IA64_PCREL21B 0x49
#define R_IA64_PCREL64LSB 0x4f
#define R_IA64_LTOFF_FPTR22 0x52
#define R_IA64_SEGREL64LSB 0x5f
#define R_IA64_LTOFF22X 0x86
#define R_IA64_LDXMOV 0x87
#endif
