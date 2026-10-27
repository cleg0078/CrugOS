/* Minimal ELF64 loader API shared by the UEFI loader and host tests. */
#ifndef CRUG_LOADER_ELF_H
#define CRUG_LOADER_ELF_H

#include <crug/types.h>

#define ELF_PT_LOAD 1u
#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define ELF_PF_R 4u

#define ELF_MACHINE_X86_64 62u
#define ELF_CLASS_64 2u
#define ELF_DATA_LSB 1u

typedef struct elf64_ehdr {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf64_ehdr;

typedef struct elf64_phdr {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
} elf64_phdr;

_Static_assert(sizeof(elf64_ehdr) == 64, "ELF64 header layout");
_Static_assert(sizeof(elf64_phdr) == 56, "ELF64 program header layout");

typedef struct elf_ops {
    /* Opaque context passed to every callback. */
    void *ctx;
    /* Reserve [addr, addr+size) of physical memory; executable is 1 for PF_X. */
    int (*reserve)(void *ctx, u64 addr, u64 size, int executable);
    /* Copy file bytes to physical address dst. */
    int (*copy)(void *ctx, u64 dst, const void *src, u64 n);
    /* Zero bss range at physical address dst. */
    int (*zero)(void *ctx, u64 dst, u64 n);
    /* Report a static reason string. */
    void (*err)(void *ctx, const char *msg);
} elf_ops;

typedef struct elf_image {
    u64 entry;
    u64 base;
    u64 size;
} elf_image;

/* Validate and load all PT_LOAD segments. Returns 0 on success. */
int elf_load64(const void *data, u64 size, const elf_ops *ops, elf_image *out);

#endif /* CRUG_LOADER_ELF_H */
