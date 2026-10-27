/* Seed material for the kernel: EFI RNG, else RDRAND, else timing jitter. */
#include <crug/types.h>
#include "uefi.h"
#include "platform.h"

static u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static int cpu_has_rdrand(void)
{
    u32 a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    (void)b;
    (void)d;
    return (c & (1u << 30)) != 0;
}

static int rdrand64(u64 *value)
{
    u8 ok = 0;
    int i;
    for (i = 0; i < 10; i++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(*value), "=qm"(ok));
        if (ok) {
            return 0;
        }
    }
    return -1;
}

static u64 mix(u64 x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

void loader_entropy_fill(efi_system_table *st, u8 out[64])
{
    efi_rng_protocol *rng = 0;
    u32 i;

    if (st && st->boot_services) {
        efi_status status = st->boot_services->locate_protocol((efi_guid *)&g_guid_rng, 0,
                                                               (void **)&rng);
        if (!EFI_ERROR(status) && rng) {
            status = rng->get_rng(rng, 0, 64, out);
            if (!EFI_ERROR(status)) {
                return;
            }
        }
    }

    if (cpu_has_rdrand()) {
        int ok = 1;
        for (i = 0; i < 8; i++) {
            u64 value = 0;
            u32 j;
            if (rdrand64(&value) != 0) {
                ok = 0;
                break;
            }
            for (j = 0; j < 8; j++) {
                out[i * 8 + j] = (u8)(value >> (8 * j));
            }
        }
        if (ok) {
            return;
        }
    }

    {
        u64 seed = rdtsc() ^ (u64)(uintptr)out ^ (u64)(uintptr)&seed;
        for (i = 0; i < 64; i++) {
            seed = mix(seed + rdtsc() + i);
            out[i] = (u8)(seed >> 32);
        }
    }
}
