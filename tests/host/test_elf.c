#include <stdio.h>
#include <crug/string.h>
#include <crug/types.h>
#include "elf.h"
#include "test.h"

#define ARENA_BASE 0x400000ull
#define ARENA_SIZE 0x2000u

static u8 arena[ARENA_SIZE];
static int error_reports;
static char last_error[64];

static int op_reserve(void *ctx, u64 addr, u64 size, int executable)
{
    (void)ctx;
    (void)executable;
    if (addr < ARENA_BASE || addr + size > ARENA_BASE + ARENA_SIZE) {
        return -1;
    }
    return 0;
}

static int op_copy(void *ctx, u64 dst, const void *src, u64 n)
{
    (void)ctx;
    if (dst < ARENA_BASE || dst + n > ARENA_BASE + ARENA_SIZE) {
        return -1;
    }
    memcpy(arena + (dst - ARENA_BASE), src, n);
    return 0;
}

static int op_zero(void *ctx, u64 dst, u64 n)
{
    (void)ctx;
    if (dst < ARENA_BASE || dst + n > ARENA_BASE + ARENA_SIZE) {
        return -1;
    }
    memset(arena + (dst - ARENA_BASE), 0, n);
    return 0;
}

static void op_err(void *ctx, const char *msg)
{
    u32 i = 0;
    (void)ctx;
    error_reports++;
    while (msg && msg[i] && i + 1 < sizeof(last_error)) {
        last_error[i] = msg[i];
        i++;
    }
    last_error[i] = 0;
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

static void build_image(u8 *image, u64 image_size)
{
    u8 *ehdr = image;
    u8 *phdr = image + 64;
    u32 i;

    memset(image, 0, image_size);
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
    put_u64(ehdr + 24, ARENA_BASE + 0x10);
    put_u64(ehdr + 32, 64);
    put_u16(ehdr + 52, 64);
    put_u16(ehdr + 54, 56);
    put_u16(ehdr + 56, 2);

    put_u32(phdr + 0, ELF_PT_LOAD);
    put_u32(phdr + 4, ELF_PF_R | ELF_PF_X);
    put_u64(phdr + 8, 0x100);
    put_u64(phdr + 16, ARENA_BASE);
    put_u64(phdr + 24, ARENA_BASE);
    put_u64(phdr + 32, 0x20);
    put_u64(phdr + 40, 0x20);
    put_u64(phdr + 48, 0x1000);

    phdr += 56;
    put_u32(phdr + 0, ELF_PT_LOAD);
    put_u32(phdr + 4, ELF_PF_R | ELF_PF_W);
    put_u64(phdr + 8, 0x180);
    put_u64(phdr + 16, ARENA_BASE + 0x1000);
    put_u64(phdr + 24, ARENA_BASE + 0x1000);
    put_u64(phdr + 32, 0x10);
    put_u64(phdr + 40, 0x40);
    put_u64(phdr + 48, 0x1000);

    for (i = 0; i < 0x20; i++) {
        image[0x100 + i] = (u8)(0x40 + i);
    }
    for (i = 0; i < 0x10; i++) {
        image[0x180 + i] = (u8)(0x80 + i);
    }
}

void test_elf_loader(void)
{
    static u8 image[0x400];
    elf_image loaded;
    int result;

    memset(arena, 0xaa, sizeof(arena));
    error_reports = 0;
    build_image(image, sizeof(image));

    result = elf_load64(image, sizeof(image), &ops, &loaded);
    if (result != 0) {
        fprintf(stderr, "  elf_load64 failed: %s\n", last_error);
    }
    CHECK(result == 0);
    CHECK(loaded.entry == ARENA_BASE + 0x10);
    CHECK(loaded.base == ARENA_BASE);
    CHECK(loaded.size == 0x1040);

    CHECK(arena[0] == 0x40);
    CHECK(arena[0x1f] == 0x5f);
    CHECK(arena[0x1000] == 0x80);
    CHECK(arena[0x100f] == 0x8f);
    CHECK(arena[0x1010] == 0x00);
    CHECK(arena[0x103f] == 0x00);

    result = elf_load64(image, 32, &ops, &loaded);
    CHECK(result != 0);

    image[0] = 0;
    result = elf_load64(image, sizeof(image), &ops, &loaded);
    CHECK(result != 0);
    CHECK(error_reports > 0);
}
