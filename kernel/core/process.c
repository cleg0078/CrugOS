/* CrugOS processes and handle tables. */
#include <crug/process.h>
#include <crug/heap.h>
#include <crug/string.h>
#include <crug/kernel.h>

static process *g_kernel_process;

void handle_table_init(handle_table *table)
{
    u32 i;

    for (i = 0; i < HANDLE_TABLE_SIZE; i++) {
        table->entries[i].type = HANDLE_FREE;
        table->entries[i].rights = 0;
        table->entries[i].object = 0;
    }
}

u64 handle_alloc(handle_table *table, u32 type, void *object, u32 rights)
{
    u32 i;

    if (!table || type == HANDLE_FREE)
        return HANDLE_NONE;
    for (i = 0; i < HANDLE_TABLE_SIZE; i++) {
        if (table->entries[i].type == HANDLE_FREE) {
            table->entries[i].type = type;
            table->entries[i].rights = rights;
            table->entries[i].object = object;
            return (u64)i + 1ull;
        }
    }
    return HANDLE_NONE;
}

void *handle_get(handle_table *table, u64 handle, u32 type)
{
    u32 idx;

    if (!table || handle == 0 || handle > HANDLE_TABLE_SIZE)
        return 0;
    idx = (u32)(handle - 1ull);
    if (table->entries[idx].type != type)
        return 0;
    return table->entries[idx].object;
}

int handle_close(handle_table *table, u64 handle)
{
    u32 idx;

    if (!table || handle == 0 || handle > HANDLE_TABLE_SIZE)
        return -1;
    idx = (u32)(handle - 1ull);
    if (table->entries[idx].type == HANDLE_FREE)
        return -1;
    table->entries[idx].type = HANDLE_FREE;
    table->entries[idx].rights = 0;
    table->entries[idx].object = 0;
    return 0;
}

void process_init(void)
{
    g_kernel_process = (process *)kzalloc(sizeof(process));
    if (!g_kernel_process)
        panic("process: cannot allocate kernel process");
    g_kernel_process->pid = 1;
    memcpy(g_kernel_process->name, "kernel", 6);
    handle_table_init(&g_kernel_process->handles);
}

process *process_current(void)
{
    return g_kernel_process;
}

handle_table *process_handles(process *p)
{
    return p ? &p->handles : 0;
}

u64 process_handle_clone(handle_table *table, u32 type, void *object, u32 rights)
{
    return handle_alloc(table, type, object, rights);
}
