/* Host-side tests for the user-program load policy (phase U1).

   test_elf.c already covers the ELF64 parser. This file proves the policy the
   exec path applies to every reservation: only page-aligned segments inside
   the user window may be reserved, and an executable segment is never
   reserved as writable (W^X). The mock reserve hook runs exactly the check
   kernel/exec/elf_exec.c runs. */
#include <stdio.h>
#include <crug/harden.h>
#include <crug/string.h>
#include <crug/types.h>
#include <crug/vm.h>
#include "elf.h"
#include "test.h"

#define TEXT_VA (USER_BASE + 0x10000ull)
#define DATA_VA (USER_BASE + 0x11000ull)
#define MAX_RESERVATIONS 8

typedef struct reservation {
    u64 addr;
    u64 size;
    int executable;
} reservation;

static u8 image[0x400];
static reservation reservations[MAX_RESERVATIONS];
static int reservation_count;
static int error_reports;

static int op_reserve(void *ctx, u64 addr, u64 size, int executable)
{
    (void)ctx;
    if (reservation_count < MAX_RESERVATIONS) {
        reservations[reservation_count].addr = addr;
        reservations[reservation_count].size = size;
        reservations[reservation_count].executable = executable;
        reservation_count++;
    }
    return harden_user_segment_ok(addr, size) ? 0 : -1;
}

static int op_copy(void *ctx, u64 dst, const void *src, u64 n)
{
    (void)ctx;
    (void)dst;
    (void)src;
    (void)n;
    return 0;
}

static int op_zero(void *ctx, u64 dst, u64 n)
{
    (void)ctx;
    (void)dst;
    (void)n;
    return 0;
}

static void op_err(void *ctx, const char *msg)
{
    (void)ctx;
    (void)msg;
    error_reports++;
}

static const elf_ops ops = {
    .ctx = 0,
    .reserve = op_reserve,
    .copy = op_copy,
    .zero = op_zero,
    .err = op_err,
};

static void put_u16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void put_u64(u8 *p, u64 v)
{
    put_u32(p, (u32)v);
    put_u32(p + 4, (u32)(v >> 32));
}

/* One R+X text segment and one R+W data segment, both with file contents. */
static void build(u64 text_va, u64 text_memsz, u64 data_va, u64 data_memsz,
                  u64 entry)
{
    u8 *ehdr = image;
    u8 *phdr = image + 64;
    u32 i;

    memset(image, 0, sizeof(image));
    reservation_count = 0;
    error_reports = 0;

    ehdr[0] = 0x7f;
    ehdr[1] = 'E';
    ehdr[2] = 'L';
    ehdr[3] = 'F';
    ehdr[4] = ELF_CLASS_64;
    ehdr[5] = ELF_DATA_LSB;
    ehdr[6] = 1;
    put_u16(ehdr + 16, 2);
    put_u16(ehdr + 18, ELF_MACHINE_X86_64);
    put_u32(ehdr + 20, 1);
    put_u64(ehdr + 24, entry);
    put_u64(ehdr + 32, 64);
    put_u16(ehdr + 52, 64);
    put_u16(ehdr + 54, 56);
    put_u16(ehdr + 56, 2);

    put_u32(phdr + 0, ELF_PT_LOAD);
    put_u32(phdr + 4, ELF_PF_R | ELF_PF_X);
    put_u64(phdr + 8, 0x100);
    put_u64(phdr + 16, text_va);
    put_u64(phdr + 24, text_va);
    put_u64(phdr + 32, 0x60);
    put_u64(phdr + 40, text_memsz);
    put_u64(phdr + 48, 0x1000);

    phdr += 56;
    put_u32(phdr + 0, ELF_PT_LOAD);
    put_u32(phdr + 4, ELF_PF_R | ELF_PF_W);
    put_u64(phdr + 8, 0x200);
    put_u64(phdr + 16, data_va);
    put_u64(phdr + 24, data_va);
    put_u64(phdr + 32, 0x10);
    put_u64(phdr + 40, data_memsz);
    put_u64(phdr + 48, 0x1000);

    for (i = 0; i < 0x60; i++) {
        image[0x100 + i] = (u8)(0x40 + i);
    }
    for (i = 0; i < 0x10; i++) {
        image[0x200 + i] = (u8)(0x80 + i);
    }
}

void test_exec_functions(void)
{
    elf_image loaded;
    int result = -1;

    build(TEXT_VA, 0x60, DATA_VA, 0x40, TEXT_VA);
    result = elf_load64(image, sizeof(image), &ops, &loaded);
    CHECK(result == 0);
    CHECK(error_reports == 0);
    CHECK(reservation_count == 2);
    CHECK(reservations[0].addr == TEXT_VA);
    CHECK(reservations[0].size == USER_PAGE);
    CHECK(reservations[0].executable == 1);
    CHECK(reservations[1].addr == DATA_VA);
    CHECK(reservations[1].size == USER_PAGE);
    CHECK(reservations[1].executable == 0);
    CHECK(loaded.entry == TEXT_VA);
    CHECK(loaded.base == TEXT_VA);
    CHECK(loaded.size == 0x1040);
    CHECK(harden_user_entry_ok(loaded.entry) == 1);
    CHECK(loaded.base >= USER_BASE);
    CHECK(loaded.base + loaded.size <= USER_TOP);

    /* A kernel address in a PT_LOAD is refused. */
    build(0x00100000ull, 0x60, DATA_VA, 0x40, 0x00100000ull);
    CHECK(elf_load64(image, sizeof(image), &ops, &loaded) != 0);
    CHECK(error_reports > 0);

    /* An unaligned segment address is refused before any reservation. */
    build(TEXT_VA + 1, 0x60, DATA_VA, 0x40, TEXT_VA + 1);
    CHECK(elf_load64(image, sizeof(image), &ops, &loaded) != 0);

    /* A segment below the window is refused by the reserve policy. */
    build(USER_BASE - USER_PAGE, 0x60, DATA_VA, 0x40, USER_BASE - USER_PAGE);
    CHECK(elf_load64(image, sizeof(image), &ops, &loaded) != 0);

    /* A segment that starts inside the window but runs past the top is
       refused as well. */
    build(USER_TOP - USER_PAGE, 0x2000, DATA_VA, 0x40, USER_TOP - USER_PAGE);
    CHECK(elf_load64(image, sizeof(image), &ops, &loaded) != 0);

    /* The linker emits an empty R+W PT_LOAD at vaddr 0 for our programs; the
       loader skips it and it must not become a reservation. */
    build(TEXT_VA, 0x60, 0, 0, TEXT_VA);
    result = elf_load64(image, sizeof(image), &ops, &loaded);
    CHECK(result == 0);
    CHECK(reservation_count == 1);
    CHECK(reservations[0].addr == TEXT_VA);
    CHECK(loaded.entry == TEXT_VA);
}
