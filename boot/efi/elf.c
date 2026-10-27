/* Minimal ELF64 program loader (freestanding, host-testable). */
#include <crug/types.h>
#include "elf.h"

#define PAGE_SIZE 4096ull
#define MAX_PHNUM 32u

static int fail(const elf_ops *ops, const char *msg)
{
    if (ops && ops->err) {
        ops->err(ops->ctx, msg);
    }
    return -1;
}

int elf_load64(const void *data, u64 size, const elf_ops *ops, elf_image *out)
{
    const u8 *base = (const u8 *)data;
    const elf64_ehdr *eh;
    u64 i;
    u64 lo = ~0ull;
    u64 hi = 0;

    if (!data || !ops || !out || !ops->reserve || !ops->copy || !ops->zero) {
        return fail(ops, "invalid loader arguments");
    }
    if (size < sizeof(elf64_ehdr)) {
        return fail(ops, "image smaller than an ELF64 header");
    }
    eh = (const elf64_ehdr *)data;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F') {
        return fail(ops, "missing ELF magic");
    }
    if (eh->ident[4] != ELF_CLASS_64) {
        return fail(ops, "not an ELF64 image");
    }
    if (eh->ident[5] != ELF_DATA_LSB) {
        return fail(ops, "only little-endian ELF is supported");
    }
    if (eh->machine != ELF_MACHINE_X86_64) {
        return fail(ops, "ELF machine is not x86_64");
    }
    if (eh->phentsize != sizeof(elf64_phdr)) {
        return fail(ops, "unexpected program header size");
    }
    if (eh->phnum == 0 || eh->phnum > MAX_PHNUM) {
        return fail(ops, "unsupported program header count");
    }
    if (eh->phoff < sizeof(elf64_ehdr) || eh->phoff > size ||
        (u64)eh->phnum * sizeof(elf64_phdr) > size - eh->phoff) {
        return fail(ops, "program header table outside the image");
    }

    for (i = 0; i < eh->phnum; i++) {
        const elf64_phdr *ph =
            (const elf64_phdr *)(base + eh->phoff + i * sizeof(elf64_phdr));
        u64 end;

        if (ph->type != ELF_PT_LOAD || ph->memsz == 0) {
            continue;
        }
        if (ph->filesz > ph->memsz) {
            return fail(ops, "segment file size exceeds memory size");
        }
        if (ph->offset > size || ph->filesz > size - ph->offset) {
            return fail(ops, "segment contents outside the image");
        }
        if (ph->vaddr & (PAGE_SIZE - 1)) {
            return fail(ops, "segment virtual address is not page aligned");
        }
        if (ph->memsz > ~0ull - ph->vaddr) {
            return fail(ops, "segment address overflow");
        }
        end = ph->vaddr + ph->memsz;

        if (ops->reserve(ops->ctx, ph->vaddr, ALIGN_UP(ph->memsz, PAGE_SIZE),
                         (ph->flags & ELF_PF_X) != 0) != 0) {
            return fail(ops, "could not reserve segment memory");
        }
        if (ph->filesz && ops->copy(ops->ctx, ph->vaddr, base + ph->offset, ph->filesz) != 0) {
            return fail(ops, "could not copy segment contents");
        }
        if (ph->memsz > ph->filesz &&
            ops->zero(ops->ctx, ph->vaddr + ph->filesz, ph->memsz - ph->filesz) != 0) {
            return fail(ops, "could not zero segment bss");
        }

        if (ph->vaddr < lo) {
            lo = ph->vaddr;
        }
        if (end > hi) {
            hi = end;
        }
    }

    if (lo == ~0ull) {
        return fail(ops, "no loadable segments");
    }
    if (eh->entry < lo || eh->entry >= hi) {
        return fail(ops, "entry point outside loaded segments");
    }

    out->entry = eh->entry;
    out->base = lo;
    out->size = hi - lo;
    return 0;
}
