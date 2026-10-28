/* CrugOS user program loader: ELF64 -> ring 3 thread (phase U1). */
#include <crug/exec.h>
#include <crug/vm.h>
#include <crug/mm.h>
#include <crug/heap.h>
#include <crug/thread.h>
#include <crug/storage.h>
#include <crug/ext4.h>
#include <crug/harden.h>
#include <crug/kernel.h>
#include <crug/string.h>
#include "elf.h"

#define PAGE_MASK (CRUG_PAGE_SIZE - 1ull)

typedef struct exec_ctx {
    thread_user_map maps[THREAD_USER_MAP_MAX];
    u32 count;
    const char *error;
} exec_ctx;

static int reserve_segment(exec_ctx *ctx, u64 va, u64 size, int executable)
{
    u64 pages;
    u64 flags;

    /* An empty PT_LOAD carries no memory (the linker emits one for a
       program without data), so it needs no reservation. */
    if (size == 0)
        return 0;
    if (ctx->count >= THREAD_USER_MAP_MAX) {
        ctx->error = "too many segments";
        return -1;
    }
    if (!harden_user_segment_ok(va, size)) {
        ctx->error = "segment outside the user window";
        return -1;
    }
    pages = size / CRUG_PAGE_SIZE;
    flags = VM_PRESENT | VM_USER;
    if (!executable)
        flags |= VM_WRITE | VM_NX;
    if (vm_user_map_pages(va, pages, flags) != 0) {
        ctx->error = "cannot map segment";
        return -1;
    }
    ctx->maps[ctx->count].va = va;
    ctx->maps[ctx->count].pages = (u32)pages;
    ctx->maps[ctx->count].pad = 0;
    ctx->count++;
    return 0;
}

/* Segments are copied through the kernel's identity map, never through the
   user page itself, so SMAP can never trip on a loader write. */
static int write_segment(u64 dst, const void *src, u64 n, int zero)
{
    while (n != 0) {
        u64 off = dst & PAGE_MASK;
        u64 chunk = CRUG_PAGE_SIZE - off;
        u64 pa = vm_translate(dst);

        if (chunk > n)
            chunk = n;
        if (pa == 0)
            return -1;
        if (zero)
            memset(pmm_phys_to_virt(pa), 0, (usize)chunk);
        else
            memcpy(pmm_phys_to_virt(pa), src, (usize)chunk);
        dst += chunk;
        if (!zero)
            src = (const u8 *)src + chunk;
        n -= chunk;
    }
    return 0;
}

static int ctx_reserve(void *opaque, u64 addr, u64 size, int executable)
{
    return reserve_segment((exec_ctx *)opaque, addr, size, executable);
}

static int ctx_copy(void *opaque, u64 dst, const void *src, u64 n)
{
    exec_ctx *ctx = (exec_ctx *)opaque;

    if (write_segment(dst, src, n, 0) != 0) {
        ctx->error = "cannot copy a segment";
        return -1;
    }
    return 0;
}

static int ctx_zero(void *opaque, u64 dst, u64 n)
{
    exec_ctx *ctx = (exec_ctx *)opaque;

    if (write_segment(dst, 0, n, 1) != 0) {
        ctx->error = "cannot zero a segment";
        return -1;
    }
    return 0;
}

static void ctx_err(void *opaque, const char *msg)
{
    ((exec_ctx *)opaque)->error = msg;
}

static void exec_unwind(exec_ctx *ctx)
{
    u32 i;

    for (i = 0; i < ctx->count; i++)
        vm_user_unmap_pages(ctx->maps[i].va, (u64)ctx->maps[i].pages);
    ctx->count = 0;
}

thread *elf_exec_run(const char *path, elf_exec_result *out)
{
    struct ext4_fs *fs = storage_root();
    elf_ops ops;
    elf_image image;
    exec_ctx ctx;
    thread *t;
    u8 *blob;
    u64 size = 0;
    u64 stack_va;

    if (!fs || !path || !out)
        return 0;
    blob = (u8 *)kmalloc(ELF_MAX_BYTES);
    if (!blob)
        return 0;
    if (ext4_read_file(fs, path, blob, ELF_MAX_BYTES, &size) != 0 || size == 0) {
        kfree(blob);
        return 0;
    }
    memset(&ctx, 0, sizeof(ctx));
    memset(&image, 0, sizeof(image));
    ops.ctx = &ctx;
    ops.reserve = ctx_reserve;
    ops.copy = ctx_copy;
    ops.zero = ctx_zero;
    ops.err = ctx_err;
    if (elf_load64(blob, size, &ops, &image) != 0) {
        kprintf("CRUGOS: exec %s failed (%s)\n", path,
                ctx.error ? ctx.error : "bad ELF image");
        exec_unwind(&ctx);
        kfree(blob);
        return 0;
    }
    kfree(blob);
    stack_va = USER_STACK_TOP - (u64)USER_STACK_PAGES * CRUG_PAGE_SIZE;
    if (reserve_segment(&ctx, stack_va, (u64)USER_STACK_PAGES * CRUG_PAGE_SIZE,
                        0) != 0) {
        kprintf("CRUGOS: exec %s failed (%s)\n", path,
                ctx.error ? ctx.error : "no user stack");
        exec_unwind(&ctx);
        return 0;
    }
    t = thread_create_user(path, image.entry, USER_STACK_TOP - 8ull, ctx.maps,
                           ctx.count, SCHED_NORMAL);
    if (!t) {
        kprintf("CRUGOS: exec %s failed (cannot create a user thread)\n", path);
        exec_unwind(&ctx);
        return 0;
    }
    out->tid = t->id;
    out->reserved = 0;
    out->entry = image.entry;
    out->base = image.base;
    out->size = image.size;
    return t;
}
