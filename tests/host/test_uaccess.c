/* Host-side tests for the CPL3 memory policy (phase U1).

   The copy paths need a real user page table, so the guest proves them with
   make test-user. Everything that is pure arithmetic is proven here: the
   window check, the per-segment policy the ELF exec path applies and the
   entry-point check. */
#include <stdio.h>
#include <crug/harden.h>
#include <crug/types.h>
#include <crug/uaccess.h>
#include <crug/vm.h>
#include "test.h"

void test_uaccess_functions(void)
{
    char buf[8] = {0};
    const u64 page = USER_PAGE;

    /* harden_user_range_ok: [USER_BASE, USER_TOP) only; kernel pointers never. */
    CHECK(harden_user_range_ok(0, 0) == 1);
    CHECK(harden_user_range_ok(0x00100000ull, 0) == 1);
    CHECK(harden_user_range_ok(0x00100000ull, 4) == 0);
    CHECK(harden_user_range_ok(0x7fffffffull, 1) == 0);
    CHECK(harden_user_range_ok(USER_BASE - 1ull, 1) == 0);
    CHECK(harden_user_range_ok(USER_BASE, 1) == 1);
    CHECK(harden_user_range_ok(USER_BASE, page) == 1);
    CHECK(harden_user_range_ok(USER_TOP - 1ull, 1) == 1);
    CHECK(harden_user_range_ok(USER_TOP, 1) == 0);
    CHECK(harden_user_range_ok(USER_TOP - 8ull, 9) == 0);
    CHECK(harden_user_range_ok(USER_TOP - 8ull, 8) == 1);
    CHECK(harden_user_range_ok(~0ull, 2) == 0);
    CHECK(harden_user_range_ok(USER_TOP - 1ull, ~0ull) == 0);

    /* harden_user_segment_ok: empty segments are free, others must be page
       aligned and inside the window. */
    CHECK(harden_user_segment_ok(0, 0) == 1);
    CHECK(harden_user_segment_ok(USER_BASE + page, 0) == 1);
    CHECK(harden_user_segment_ok(0x00100000ull, page) == 0);
    CHECK(harden_user_segment_ok(USER_BASE - page, page) == 0);
    CHECK(harden_user_segment_ok(USER_BASE + 1, page) == 0);
    CHECK(harden_user_segment_ok(USER_BASE + page, page) == 1);
    CHECK(harden_user_segment_ok(USER_TOP - page, page) == 1);
    CHECK(harden_user_segment_ok(USER_TOP - page, page + 1) == 0);
    CHECK(harden_user_segment_ok(USER_TOP, page) == 0);
    CHECK(harden_user_segment_ok(USER_TOP - page, ~0ull) == 0);

    /* harden_user_entry_ok: entry point and user stack pointer. */
    CHECK(harden_user_entry_ok(0) == 0);
    CHECK(harden_user_entry_ok(USER_BASE - 1ull) == 0);
    CHECK(harden_user_entry_ok(USER_BASE) == 1);
    CHECK(harden_user_entry_ok(USER_TOP - 1ull) == 1);
    CHECK(harden_user_entry_ok(USER_TOP) == 0);
    CHECK(harden_user_entry_ok(0x0000008040000000ull) == 1);

    /* uaccess_check is the same rule with the syscall error code. */
    CHECK(uaccess_check(USER_BASE, page) == 0);
    CHECK(uaccess_check(USER_BASE, 0) == 0);
    CHECK(uaccess_check(0x00100000ull, 8) == UACCESS_EFAULT);
    CHECK(uaccess_check(USER_TOP, 1) == UACCESS_EFAULT);
    CHECK(uaccess_check(USER_TOP - 4ull, 8) == UACCESS_EFAULT);

    /* Rejection happens before any copy, so these never touch memory. */
    CHECK(uaccess_user_read(buf, 0x00100000ull, 4) == UACCESS_EFAULT);
    CHECK(uaccess_user_read(buf, USER_TOP, 1) == UACCESS_EFAULT);
    CHECK(uaccess_user_read(0, USER_BASE, 4) == UACCESS_EFAULT);
    CHECK(uaccess_user_write(0x00100000ull, buf, 4) == UACCESS_EFAULT);
    CHECK(uaccess_user_write(USER_TOP, buf, 1) == UACCESS_EFAULT);
    CHECK(uaccess_user_write(USER_BASE, 0, 4) == UACCESS_EFAULT);
    CHECK(uaccess_user_read(buf, USER_BASE, 0) == 0);
    CHECK(uaccess_user_write(USER_BASE, buf, 0) == 0);

    /* A string without capacity, or with a bad pointer, is EFAULT and leaves
       an empty kernel buffer behind. */
    CHECK(uaccess_user_str(buf, USER_BASE, 0) == UACCESS_EFAULT);
    CHECK(uaccess_user_str(0, USER_BASE, 8) == UACCESS_EFAULT);
    CHECK(uaccess_user_str(buf, 0x00100000ull, 8) == UACCESS_EFAULT);
    CHECK(buf[0] == 0);
}
