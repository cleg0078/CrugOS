/* CrugOS threads and scheduler. */
#ifndef CRUG_THREAD_H
#define CRUG_THREAD_H

#include <crug/types.h>
#include <crug/compiler.h>
#include <crug/vm.h>

typedef enum thread_state {
    THREAD_RUNNING = 0,
    THREAD_RUNNABLE,
    THREAD_BLOCKED,
    THREAD_SLEEPING,
    THREAD_STOPPED,
    THREAD_ZOMBIE
} thread_state;

typedef enum sched_class {
    SCHED_IDLE = 0,
    SCHED_NORMAL,
    SCHED_INTERACTIVE,
    SCHED_REALTIME_SYSTEM
} sched_class;

#define THREAD_NAME_MAX 32u
#define THREAD_STACK_PAGES 4u
#define THREAD_GUARD_PAGES 1u
#define SCHED_CLASSES 4u

#include <crug/ufile.h>

/* A thread that enters ring 3 owns a private address space (phase U2). */
#define THREAD_FLAG_USER 1u

typedef struct wait_queue {
    struct thread *head;
    struct thread *tail;
} wait_queue;

typedef struct thread {
    u64 rsp;
    u32 id;
    u32 cpu;
    thread_state state;
    sched_class class;
    u32 quantum;
    u32 reserved;
    char name[THREAD_NAME_MAX];
    void (*entry)(void *arg);
    void *arg;
    u64 stack_base;
    u64 stack_size;
    struct thread *run_next;
    struct thread *wait_next;
    struct thread *sleep_next;
    struct thread *all_next;
    wait_queue joiners;
    u64 wake_tick;
    i64 exit_code;
    /* Ring 3 state; only meaningful when flags has THREAD_FLAG_USER. */
    u64 user_rip;
    u64 user_rsp;
    u32 flags;
    u32 space_zero;
    /* Private address space; 0 for every kernel thread. */
    vm_space *space;
    /* Open files: fds 3..10 index this table (phase U3). */
    ufile_table files;
} thread;

void sched_init(void);
void sched_start(void);
thread *thread_create(const char *name, void (*entry)(void *), void *arg,
                      sched_class cls);
thread *thread_create_user(const char *name, u64 entry, u64 user_rsp,
                           vm_space *space, sched_class cls);
int thread_join(thread *t);
i64 thread_join_code(thread *t);
void thread_exit(void) CRUG_NORETURN;
void thread_kill_current(i64 code) CRUG_NORETURN;
void sched_yield(void);
void sched_tick(void);
thread *sched_current(void);
u64 sched_switches(void);
u32 thread_id(thread *t);
const char *thread_name(thread *t);

void wait_queue_init(wait_queue *wq);
void sched_block(wait_queue *wq);
void sched_wake(thread *t);
void wait_queue_wake_one(wait_queue *wq);
void wait_queue_wake_all(wait_queue *wq);
void sched_sleep_ticks(u64 ticks);

int sched_selftest(void);

#endif /* CRUG_THREAD_H */
