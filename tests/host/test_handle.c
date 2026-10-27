/* Host tests for process handle tables. */
#include <crug/process.h>
#include <crug/heap.h>
#include <crug/kernel.h>
#include "test.h"

/* process_init needs the kernel allocator; keep the host test freestanding. */
static u8 g_process_store[8192];

void *kzalloc(usize size)
{
    if (size > sizeof(g_process_store))
        return 0;
    return g_process_store;
}

void panic(const char *fmt, ...)
{
    (void)fmt;
    for (;;)
        __builtin_trap();
}

void test_handle_functions(void)
{
    static handle_table table;
    static handle_table full;
    process *p;
    u64 h;
    u32 i;
    int failed;

    handle_table_init(&table);
    for (i = 0; i < HANDLE_TABLE_SIZE; i++)
        CHECK(table.entries[i].type == HANDLE_FREE);

    h = handle_alloc(&table, HANDLE_THREAD, (void *)0x1000, HANDLE_RIGHT_ALL);
    CHECK(h == 1);
    CHECK(handle_get(&table, h, HANDLE_THREAD) == (void *)0x1000);
    CHECK(handle_get(&table, h, HANDLE_PORT) == 0);
    CHECK(table.entries[0].rights == HANDLE_RIGHT_ALL);

    CHECK(handle_alloc(&table, HANDLE_PORT, (void *)0x2000, HANDLE_RIGHT_READ) == 2);
    CHECK(handle_get(&table, 2, HANDLE_PORT) == (void *)0x2000);
    CHECK(handle_get(&table, 0, HANDLE_PORT) == 0);
    CHECK(handle_get(&table, HANDLE_TABLE_SIZE + 1, HANDLE_PORT) == 0);

    CHECK(handle_close(&table, 0) == -1);
    CHECK(handle_close(&table, HANDLE_TABLE_SIZE + 1) == -1);
    CHECK(handle_close(&table, 1) == 0);
    CHECK(handle_close(&table, 1) == -1);
    CHECK(handle_get(&table, 1, HANDLE_THREAD) == 0);

    h = handle_alloc(&table, HANDLE_PROCESS, (void *)0x3000, HANDLE_RIGHT_WRITE);
    CHECK(h == 1);

    handle_table_init(&full);
    for (i = 0; i < HANDLE_TABLE_SIZE; i++)
        CHECK(handle_alloc(&full, HANDLE_THREAD, (void *)(uintptr)(i + 1), HANDLE_RIGHT_ALL) == (u64)i + 1);
    CHECK(handle_alloc(&full, HANDLE_THREAD, (void *)0x1, HANDLE_RIGHT_ALL) == HANDLE_NONE);
    CHECK(handle_get(&full, 256, HANDLE_THREAD) == (void *)(uintptr)256);

    failed = 0;
    for (i = 0; i < HANDLE_TABLE_SIZE; i++)
        if (handle_close(&full, (u64)i + 1) != 0)
            failed = 1;
    CHECK(failed == 0);
    CHECK(handle_alloc(&full, HANDLE_PORT, (void *)0x4000, HANDLE_RIGHT_READ) == 1);

    process_init();
    p = process_current();
    CHECK(p != 0);
    CHECK(p->pid == 1);
    CHECK(p->name[0] == 'k' && p->name[5] == 'l' && p->name[6] == '\0');
    CHECK(process_handles(p) == &p->handles);
    CHECK(process_handles(0) == 0);
    CHECK(process_handle_clone(&p->handles, HANDLE_THREAD, (void *)0x5000, HANDLE_RIGHT_READ) == 1);
    CHECK(handle_get(&p->handles, 1, HANDLE_THREAD) == (void *)0x5000);
}
