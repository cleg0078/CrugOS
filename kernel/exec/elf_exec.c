/* CrugOS user program loader: ELF64 -> ring 3 thread (phase U1/U2).
 *
 * Every program gets its own vm_space before a single segment is mapped, so a
 * segment that lands in the window is private to that program. Segments are
 * copied through the kernel identity map (pmm_phys_to_virt of the translated
 * page), never through the user mapping, so SMAP cannot trip the loader. */
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
    vm_space *space;
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
    if (!ctx->space) {
        ctx->error = "no address space";
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
    if (vm_space_map_user_pages(ctx->space, va, pages, flags) != 0) {
        ctx->error = "cannot map segment";
        return -1;
    }
    return 0;
}

/* Copy into (or zero) a freshly mapped user range, page chunk by page chunk,
   through the physical frame the program's own page tables point at. */
static int write_segment(exec_ctx *ctx, u64 dst, const void *src, u64 n, int zero)
{
    while (n != 0) {
        u64 off = dst & PAGE_MASK;
        u64 chunk = CRUG_PAGE_SIZE - off;
        u64 pa;

        if (chunk > n)
            chunk = n;
        pa = vm_space_translate(ctx->space, dst);
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

    if (write_segment(ctx, dst, src, n, 0) != 0) {
        ctx->error = "cannot copy a segment";
        return -1;
    }
    return 0;
}

static int ctx_zero(void *opaque, u64 dst, u64 n)
{
    exec_ctx *ctx = (exec_ctx *)opaque;

    if (write_segment(ctx, dst, 0, n, 1) != 0) {
        ctx->error = "cannot zero a segment";
        return -1;
    }
    return 0;
}

static void ctx_err(void *opaque, const char *msg)
{
    ((exec_ctx *)opaque)->error = msg;
}

/* Release every page of the new space, its page tables and the struct. */
static void exec_unwind(exec_ctx *ctx)
{
    if (!ctx->space)
        return;
    vm_space_destroy(ctx->space);
    kfree(ctx->space);
    ctx->space = 0;
}

int elf_load_user(const char *path, u32 argc, const char *const *argv,
                  elf_image_load *out)
{
    struct ext4_fs *fs = storage_root();
    elf_ops ops;
    elf_image image;
    exec_ctx ctx;
    u8 *blob;
    u8 args[EXEC_ARGS_BLOCK_MAX];
    u64 size = 0;
    u64 stack_va;
    u64 rsp_va = USER_STACK_TOP - 8ull;

    if (!fs || !path || !out)
        return -1;
    memset(out, 0, sizeof(*out));
    memset(&ctx, 0, sizeof(ctx));
    ctx.space = (vm_space *)kmalloc(sizeof(vm_space));
    if (!ctx.space)
        return -1;
    if (vm_space_create(ctx.space) != 0) {
        kfree(ctx.space);
        ctx.space = 0;
        kprintf("CRUGOS: exec %s failed (no address space)\n", path);
        return -1;
    }
    blob = (u8 *)kmalloc(ELF_MAX_BYTES);
    if (!blob) {
        exec_unwind(&ctx);
        return -1;
    }
    if (ext4_read_file(fs, path, blob, ELF_MAX_BYTES, &size) != 0 || size == 0) {
        kfree(blob);
        exec_unwind(&ctx);
        return -1;
    }
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
        return -1;
    }
    kfree(blob);
    stack_va = USER_STACK_TOP - (u64)USER_STACK_PAGES * CRUG_PAGE_SIZE;
    if (reserve_segment(&ctx, stack_va, (u64)USER_STACK_PAGES * CRUG_PAGE_SIZE,
                        0) != 0) {
        kprintf("CRUGOS: exec %s failed (%s)\n", path,
                ctx.error ? ctx.error : "no user stack");
        exec_unwind(&ctx);
        return -1;
    }
    if (argc != 0) {
        u64 area = EXEC_ARGS_BLOCK_MAX;
        u64 area_va = USER_STACK_TOP - area;
        u64 rsp_off = 0;

        /* The initial stack image lives at the very top of the user stack:
           argc, argv[], NULL, envp[], NULL, then the strings. */
        if (exec_args_build(args, area, area_va, argc, argv, 0, &rsp_off) != 0) {
            kprintf("CRUGOS: exec %s failed (argv block)\n", path);
            exec_unwind(&ctx);
            return -1;
        }
        if (write_segment(&ctx, area_va, args, (usize)area, 0) != 0) {
            kprintf("CRUGOS: exec %s failed (cannot write argv)\n", path);
            exec_unwind(&ctx);
            return -1;
        }
        rsp_va = area_va + rsp_off;
    }
    out->space = ctx.space;
    out->entry = image.entry;
    out->rsp = rsp_va;
    out->base = image.base;
    out->size = image.size;
    return 0;
}

thread *elf_spawn_user(const char *path, u32 argc, const char *const *argv,
                       u32 parent_id, elf_exec_result *out)
{
    elf_image_load img;
    thread *t;

    if (elf_load_user(path, argc, argv, &img) != 0)
        return 0;
    t = thread_create_user(path, img.entry, img.rsp, img.space, SCHED_NORMAL);
    if (!t) {
        kprintf("CRUGOS: exec %s failed (cannot create a user thread)\n", path);
        vm_space_destroy(img.space);
        kfree(img.space);
        return 0;
    }
    t->parent_id = parent_id;
    if (out) {
        out->tid = t->id;
        out->reserved = 0;
        out->entry = img.entry;
        out->base = img.base;
        out->size = img.size;
    }
    return t;
}

/* Phase U4: exec replaces the running image. The new space must be active
   BEFORE the old one is destroyed: CR3 still points at the old root, and
   vm_space_destroy frees that root page. Kernel code, the kernel stack and
   the timer are all in the shared slot 0 of both spaces, so the switch is
   safe there. */
int elf_exec_in_place(const char *path, u32 argc, const char *const *argv,
                      u64 *out_entry, u64 *out_rsp)
{
    thread *t = sched_current();
    elf_image_load img;
    vm_space *old;

    if (!t || !(t->flags & THREAD_FLAG_USER) || !out_entry || !out_rsp)
        return -1;
    if (elf_load_user(path, argc, argv, &img) != 0)
        return -1;
    old = t->space;
    t->space = img.space;
    t->user_rip = img.entry;
    t->user_rsp = img.rsp;
    vm_space_activate(img.space);
    if (old) {
        vm_space_destroy(old);
        kfree(old);
    }
    *out_entry = img.entry;
    *out_rsp = img.rsp;
    return 0;
}

thread *elf_exec_run(const char *path, elf_exec_result *out)
{
    return elf_spawn_user(path, 0, 0, 0, out);
}

thread *elf_exec_run_args(const char *path, u32 argc, const char *const *argv,
                          elf_exec_result *out)
{
    return elf_spawn_user(path, argc, argv, 0, out);
}