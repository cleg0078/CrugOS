#include <crug/kernel.h>
#include <crug/mm.h>
#include <crug/vm.h>
#include <crug/trap.h>
#include <crug/timer.h>
#include <crug/heap.h>
#include <crug/thread.h>
#include <crug/process.h>
#include <crug/msgport.h>
#include <crug/syscall.h>
#include <crug/block.h>
#include <crug/gpt.h>
#include <crug/ext4.h>
#include <crug/storage.h>
#include <crug/usb.h>
#include <crug/audio.h>
#include <crug/gfx.h>
#include <crug/workbench.h>
#include <crug/net.h>

static handle_table g_handle_test;

static int handle_selftest(void)
{
    u32 i;
    u64 h;

    handle_table_init(&g_handle_test);
    for (i = 0; i < HANDLE_TABLE_SIZE; i++) {
        h = handle_alloc(&g_handle_test, HANDLE_THREAD,
                         (void *)(uintptr)(i + 1), HANDLE_RIGHT_ALL);
        if (h != (u64)i + 1ull)
            return 0;
    }
    if (handle_alloc(&g_handle_test, HANDLE_THREAD,
                     (void *)(uintptr)1, HANDLE_RIGHT_ALL) != HANDLE_NONE)
        return 0;
    if (handle_get(&g_handle_test, 1, HANDLE_THREAD) != (void *)(uintptr)1)
        return 0;
    if (handle_get(&g_handle_test, 1, HANDLE_PORT) != 0)
        return 0;
    if (handle_close(&g_handle_test, 1) != 0)
        return 0;
    if (handle_close(&g_handle_test, 1) != -1)
        return 0;
    if (handle_get(&g_handle_test, 1, HANDLE_THREAD) != 0)
        return 0;
    for (i = 2; i <= HANDLE_TABLE_SIZE; i++)
        handle_close(&g_handle_test, (u64)i);
    return handle_get(&g_handle_test, 2, HANDLE_THREAD) == 0;
}
void selftest_run(void)
{
    if (!pmm_selftest())
        panic("selftest: physical allocator");
    if (!vm_selftest())
        panic("selftest: virtual memory");
    if (!vm_audit_wx())
        panic("selftest: W^X audit");
    if (!trap_selftest_int3())
        panic("selftest: breakpoint exception");
    if (!timer_selftest())
        panic("selftest: timer interrupt");
    if (!heap_selftest())
        panic("selftest: kernel heap");
    if (!handle_selftest())
        panic("selftest: handles");
    if (!sched_selftest())
        panic("selftest: scheduler");
    if (!msgport_selftest())
        panic("selftest: message ports");
    if (!syscall_selftest())
        panic("selftest: syscalls");
    if (!block_selftest())
        panic("selftest: block layer");
    if (!gpt_selftest())
        panic("selftest: GPT");
    if (!ext4_selftest())
        panic("selftest: ext4 structures");
    if (!storage_selftest())
        panic("selftest: storage stack");
    if (!net_selftest())
        panic("selftest: network");
    if (!usb_selftest())
        panic("selftest: usb input");
    if (!audio_selftest())
        panic("selftest: audio");
    if (!gfx_selftest())
        panic("selftest: gfx");
    if (!wb_selftest())
        panic("selftest: workbench");
}
