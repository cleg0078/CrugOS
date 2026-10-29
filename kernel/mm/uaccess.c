/* CrugOS user-memory access helpers (phase U1). */
#include <crug/uaccess.h>
#include <crug/harden.h>
#include <crug/cpu.h>
#include <crug/string.h>
#include <crug/mm.h>
#ifndef CRUG_HOST_TEST
#include <crug/vm.h>
#endif

/* SMAP forbids ring-0 accesses to U/S=1 pages unless EFLAGS.AC is set. The
   window check above is unconditional, so a CPU without SMAP is just as safe
   against kernel pointers; it only needs no stac/clac at all. */
static void uaccess_begin(void)
{
#ifndef CRUG_HOST_TEST
    if (g_cpu_features.smap)
        __asm__ volatile("stac" : : : "memory");
#endif
}

static void uaccess_end(void)
{
#ifndef CRUG_HOST_TEST
    if (g_cpu_features.smap)
        __asm__ volatile("clac" : : : "memory");
#endif
}

int uaccess_check(u64 addr, u64 len)
{
    return harden_user_range_ok(addr, len) ? 0 : UACCESS_EFAULT;
}

#ifndef CRUG_HOST_TEST
/* The window check says where user memory may live; it cannot say whether a
   page is mapped.  A user pointer is allowed to name an unmapped page, so the
   copy paths walk the range first and report EFAULT instead of taking a
   kernel #PF (the CPU would push that fault while SMAP is already the
   problem here, and there is no fixup table in this kernel). */
static int uaccess_pages_mapped(u64 addr, u64 len)
{
    const u64 mask = CRUG_PAGE_SIZE - 1ull;
    u64 end = addr + len;
    u64 page;

    if (end < addr)
        return 0;
    for (page = addr & ~mask; page < end; page += CRUG_PAGE_SIZE) {
        if (vm_translate(page) == 0)
            return 0;
    }
    return 1;
}

/* A write demands one more thing than a read (phase U6): a leaf shared with a
   forked child is mapped read-only in every space, and CR0.WP turns a ring-0
   store to it into a fatal fault. Give such a leaf a private copy first and
   flush the stale writable-less translation; the copy holds identical bytes,
   so the store below still sees exactly what the caller wrote. */
static int uaccess_pages_writable(u64 addr, u64 len)
{
    const u64 mask = CRUG_PAGE_SIZE - 1ull;
    u64 end = addr + len;
    u64 page;
    vm_space *space = vm_active_space();

    if (end < addr)
        return 0;
    for (page = addr & ~mask; page < end; page += CRUG_PAGE_SIZE) {
        if (vm_translate(page) == 0)
            return 0;
        if (space) {
            int r = vm_space_cow_fault(space, page);

            if (r < 0)
                return 0;
            if (r > 0)
                vm_flush_page(page);
        }
    }
    return 1;
}
#endif

int uaccess_user_read(void *kdst, u64 uaddr, u64 len)
{
    if (!kdst)
        return UACCESS_EFAULT;
    if (uaccess_check(uaddr, len) != 0)
        return UACCESS_EFAULT;
    if (len == 0)
        return 0;
#ifndef CRUG_HOST_TEST
    if (!uaccess_pages_mapped(uaddr, len))
        return UACCESS_EFAULT;
#endif
    uaccess_begin();
    memcpy(kdst, (const void *)(uintptr)uaddr, (usize)len);
    uaccess_end();
    return 0;
}

int uaccess_user_write(u64 uaddr, const void *ksrc, u64 len)
{
    if (!ksrc)
        return UACCESS_EFAULT;
    if (uaccess_check(uaddr, len) != 0)
        return UACCESS_EFAULT;
    if (len == 0)
        return 0;
#ifndef CRUG_HOST_TEST
    if (!uaccess_pages_writable(uaddr, len))
        return UACCESS_EFAULT;
#endif
    uaccess_begin();
    memcpy((void *)(uintptr)uaddr, ksrc, (usize)len);
    uaccess_end();
    return 0;
}

int uaccess_user_str(char *kdst, u64 uaddr, u64 cap)
{
    u64 i;

    if (!kdst || cap == 0)
        return UACCESS_EFAULT;
    for (i = 0; i + 1 < cap; i++) {
        u8 c = 0;

        if (uaccess_user_read(&c, uaddr + i, 1) != 0) {
            kdst[0] = 0;
            return UACCESS_EFAULT;
        }
        kdst[i] = (char)c;
        if (c == 0)
            return 0;
    }
    kdst[cap - 1] = 0;
    return UACCESS_EFAULT;
}
