/* CrugOS processes and process-local handles. */
#ifndef CRUG_PROCESS_H
#define CRUG_PROCESS_H

#include <crug/types.h>

#define HANDLE_TABLE_SIZE 256u
#define HANDLE_NONE 0ull

typedef enum handle_type {
    HANDLE_FREE = 0,
    HANDLE_THREAD,
    HANDLE_PORT,
    HANDLE_PROCESS,
    HANDLE_SOCKET
} handle_type;

#define HANDLE_RIGHT_READ  (1u << 0)
#define HANDLE_RIGHT_WRITE (1u << 1)
#define HANDLE_RIGHT_ALL   (HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE)

typedef struct handle_entry {
    u32 type;
    u32 rights;
    void *object;
} handle_entry;

typedef struct handle_table {
    handle_entry entries[HANDLE_TABLE_SIZE];
} handle_table;

typedef struct process {
    u32 pid;
    char name[32];
    handle_table handles;
    struct process *next;
} process;

void handle_table_init(handle_table *table);
u64 handle_alloc(handle_table *table, u32 type, void *object, u32 rights);
void *handle_get(handle_table *table, u64 handle, u32 type);
int handle_close(handle_table *table, u64 handle);

void process_init(void);
process *process_current(void);
handle_table *process_handles(process *p);
u64 process_handle_clone(handle_table *table, u32 type, void *object, u32 rights);

#endif /* CRUG_PROCESS_H */
