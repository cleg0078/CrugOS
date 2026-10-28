/* CrugOS phase 2 threads and preemptive scheduler. */
#include <crug/thread.h>
#include <crug/heap.h>
#include <crug/mm.h>
#include <crug/vm.h>
#include <crug/timer.h>
#include <crug/cpu.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include <crug/segment.h>
#include <crug/harden.h>

extern void cpu_switch(u64 *prev_rsp, u64 *next_rsp);

typedef struct run_queue {
    thread *head;
    thread *tail;
    u32 count;
} run_queue;

struct scheduler {
    thread *current;
    thread *idle;
    run_queue rq[SCHED_CLASSES];
    thread *sleep_list;
    u32 next_id;
    u64 switches;
    int started;
};

static struct scheduler g_sched;
static thread g_boot_thread;

extern void thread_user_start(void);

static const u8 g_quanta[SCHED_CLASSES] = { 1, 4, 2, 8 };

static u32 quantum_for(sched_class cls)
{
    return g_quanta[cls];
}

static void rq_push(thread *t)
{
    run_queue *q = &g_sched.rq[t->class];

    t->run_next = 0;
    if (q->tail)
        q->tail->run_next = t;
    else
        q->head = t;
    q->tail = t;
    q->count++;
}

static thread *rq_pop(sched_class cls)
{
    run_queue *q = &g_sched.rq[cls];
    thread *t = q->head;

    if (!t)
        return 0;
    q->head = t->run_next;
    if (!q->head)
        q->tail = 0;
    t->run_next = 0;
    q->count--;
    return t;
}

static sched_class highest_runnable(void)
{
    int c;

    for (c = (int)SCHED_REALTIME_SYSTEM; c >= 0; c--) {
        if (g_sched.rq[c].count != 0)
            return (sched_class)c;
    }
    return SCHED_IDLE;
}

static thread *pick_next(void)
{
    int c;

    for (c = (int)SCHED_REALTIME_SYSTEM; c >= 0; c--) {
        if (g_sched.rq[c].count != 0)
            return rq_pop((sched_class)c);
    }
    return 0;
}

static void schedule(void)
{
    thread *prev = g_sched.current;
    thread *next = pick_next();

    if (!next)
        next = g_sched.idle;
    if (!next)
        return;
    if (next == prev) {
        next->state = THREAD_RUNNING;
        return;
    }
    g_sched.current = next;
    next->state = THREAD_RUNNING;
    /* A CPL3 entry lands on TSS.rsp0, so it must already point at the incoming
       thread's kernel stack (the boot stack for the idle boot thread). */
    cpu_set_kernel_stack(next->stack_base
                             ? next->stack_base + next->stack_size
                             : cpu_boot_stack_top());
    /* Switch CR3 before the incoming thread touches its own memory. Kernel
       threads carry space == 0 and put the kernel root back in place. */
    vm_space_activate(next->space);
    g_sched.switches++;
    cpu_switch(&prev->rsp, &next->rsp);
}

static void sleep_add(thread *t)
{
    t->sleep_next = g_sched.sleep_list;
    g_sched.sleep_list = t;
}

static void sleep_remove(thread *t)
{
    thread **pp = &g_sched.sleep_list;

    while (*pp) {
        if (*pp == t) {
            *pp = t->sleep_next;
            t->sleep_next = 0;
            return;
        }
        pp = &(*pp)->sleep_next;
    }
}

static void wake_sleepers(void)
{
    u64 now = timer_ticks();
    thread **pp = &g_sched.sleep_list;

    while (*pp) {
        thread *t = *pp;

        if (now >= t->wake_tick) {
            *pp = t->sleep_next;
            t->sleep_next = 0;
            t->state = THREAD_RUNNABLE;
            t->quantum = quantum_for(t->class);
            rq_push(t);
        } else {
            pp = &t->sleep_next;
        }
    }
}

void wait_queue_init(wait_queue *wq)
{
    wq->head = 0;
    wq->tail = 0;
}

void sched_wake(thread *t)
{
    if (!t)
        return;
    if (t->state != THREAD_BLOCKED && t->state != THREAD_SLEEPING)
        return;
    if (t->state == THREAD_SLEEPING)
        sleep_remove(t);
    t->state = THREAD_RUNNABLE;
    t->quantum = quantum_for(t->class);
    rq_push(t);
}

void wait_queue_wake_one(wait_queue *wq)
{
    thread *t = wq->head;

    if (!t)
        return;
    wq->head = t->wait_next;
    if (!wq->head)
        wq->tail = 0;
    t->wait_next = 0;
    sched_wake(t);
}

void wait_queue_wake_all(wait_queue *wq)
{
    while (wq->head)
        wait_queue_wake_one(wq);
}

void sched_block(wait_queue *wq)
{
    thread *t = g_sched.current;

    if (!t)
        panic("sched: block without current thread");
    if (wq) {
        t->wait_next = 0;
        if (wq->tail)
            wq->tail->wait_next = t;
        else
            wq->head = t;
        wq->tail = t;
    }
    t->state = THREAD_BLOCKED;
    schedule();
}

void sched_yield(void)
{
    u64 flags;
    thread *t;

    flags = cpu_irq_save();
    if (!g_sched.started) {
        cpu_irq_restore(flags);
        return;
    }
    t = g_sched.current;
    if (t && t->state == THREAD_RUNNING) {
        t->state = THREAD_RUNNABLE;
        t->quantum = quantum_for(t->class);
        rq_push(t);
        schedule();
    }
    cpu_irq_restore(flags);
}

void sched_sleep_ticks(u64 ticks)
{
    u64 flags = cpu_irq_save();
    thread *t = g_sched.current;

    if (!t) {
        cpu_irq_restore(flags);
        return;
    }
    t->wake_tick = timer_ticks() + ticks;
    t->state = THREAD_SLEEPING;
    sleep_add(t);
    schedule();
    cpu_irq_restore(flags);
}

void sched_tick(void)
{
    thread *t;

    if (!g_sched.started)
        return;
    t = g_sched.current;
    if (!t)
        return;

    if (t->state == THREAD_RUNNING && t->class != SCHED_IDLE) {
        if (t->quantum > 0)
            t->quantum--;
        if (t->quantum == 0) {
            t->quantum = quantum_for(t->class);
            t->state = THREAD_RUNNABLE;
            rq_push(t);
            wake_sleepers();
            schedule();
            return;
        }
    }

    wake_sleepers();

    if (t->state == THREAD_RUNNING && highest_runnable() > t->class) {
        t->state = THREAD_RUNNABLE;
        t->quantum = quantum_for(t->class);
        rq_push(t);
        schedule();
    }
}

static void thread_entry(void)
{
    thread *t = g_sched.current;

    if (!t)
        panic("sched: thread entry without current");
    t->entry(t->arg);
    thread_exit();
}

static void thread_destroy(thread *t)
{
    if (!t)
        return;
    if (t == g_sched.current)
        panic("sched: cannot destroy current thread");
    /* A program that exits without closing its files still flushes them. */
    ufile_close_all(&t->files);
    if (t->space) {
        /* The thread is already off the CPU, so its root is not the active one
           and its tables can be walked and released page by page. */
        vm_space_destroy(t->space);
        kfree(t->space);
        t->space = 0;
    }
    if (t->stack_base) {
        /* The identity map must cover every page the PMM can hand out, so
           restore the guard page before returning the stack to the PMM. */
        if (vm_map_page(t->stack_base, t->stack_base,
                        VM_PRESENT | VM_WRITE | VM_GLOBAL | VM_NX) != 0)
            panic("sched: cannot restore stack guard");
        pmm_free_pages(t->stack_base, THREAD_STACK_PAGES + THREAD_GUARD_PAGES);
    }
    kfree(t);
}

thread *thread_create(const char *name, void (*entry)(void *), void *arg,
                      sched_class cls)
{
    thread *t;
    u64 base;
    u64 top;
    u64 *sp;
    u32 pages = THREAD_STACK_PAGES + THREAD_GUARD_PAGES;
    usize n;

    if (!entry)
        return 0;
    t = (thread *)kzalloc(sizeof(thread));
    if (!t)
        return 0;
    ufile_table_init(&t->files);
    base = pmm_alloc_pages(pages);
    if (base == 0) {
        kfree(t);
        return 0;
    }
    if (vm_unmap_page(base) != 0) {
        pmm_free_pages(base, pages);
        kfree(t);
        return 0;
    }

    t->id = g_sched.next_id++;
    t->cpu = 0;
    t->state = THREAD_RUNNABLE;
    t->class = cls;
    t->quantum = quantum_for(cls);
    n = strlen(name ? name : "thread");
    if (n >= THREAD_NAME_MAX)
        n = THREAD_NAME_MAX - 1;
    if (n)
        memcpy(t->name, name, n);
    t->name[n] = 0;
    t->entry = entry;
    t->arg = arg;
    t->stack_base = base;
    t->stack_size = (u64)pages * CRUG_PAGE_SIZE;
    wait_queue_init(&t->joiners);

    top = base + (u64)pages * CRUG_PAGE_SIZE;
    sp = (u64 *)(uintptr)(top - 72ull);
    sp[0] = 0; /* r15 */
    sp[1] = 0; /* r14 */
    sp[2] = 0; /* r13 */
    sp[3] = 0; /* r12 */
    sp[4] = 0; /* rbp */
    sp[5] = 0; /* rbx */
    sp[6] = 0x202ull; /* rflags, IF=1 */
    sp[7] = (u64)(uintptr)thread_entry;
    sp[8] = 0; /* padding so entry sees rsp % 16 == 8 */
    t->rsp = (u64)(uintptr)sp;

    rq_push(t);
    return t;
}

/* thread_create needs a kernel entry point; a user thread never runs it because
   thread_create_user swaps sp[7] for the iretq trampoline. */
static void thread_user_never(void *arg)
{
    (void)arg;
}

thread *thread_create_user(const char *name, u64 entry, u64 user_rsp,
                           vm_space *space, sched_class cls)
{
    thread *t;
    u64 *sp;

    if (!space || !space->pml4)
        return 0;
    if (!harden_user_entry_ok(entry) || !harden_user_entry_ok(user_rsp))
        return 0;
    t = thread_create(name, thread_user_never, 0, cls);
    if (!t)
        return 0;
    t->flags |= THREAD_FLAG_USER;
    t->user_rip = entry;
    t->user_rsp = user_rsp;
    t->space = space;
    sp = (u64 *)(uintptr)(t->stack_base + t->stack_size - 72ull);
    sp[7] = (u64)(uintptr)thread_user_start;
    t->rsp = (u64)(uintptr)sp;
    return t;
}


/* Called by kernel/arch/x86_64/user.S while building the ring-3 iretq frame.
   Keeps every struct thread offset out of assembly. */
void thread_user_frame(u64 *out)
{
    thread *t = g_sched.current;

    if (!t || (t->flags & THREAD_FLAG_USER) == 0)
        panic("sched: user trampoline without a user thread");
    cpu_set_kernel_stack(t->stack_base + t->stack_size);
    out[0] = t->user_rip;
    out[1] = t->user_rsp;
    out[2] = (u64)SEG_USER_DS;
    out[3] = (u64)SEG_USER_CS;
}

void thread_kill_current(i64 code)
{
    thread *t = g_sched.current;
    u64 flags = cpu_irq_save();

    if (!t)
        panic("sched: exit without current thread");
    t->exit_code = code;
    t->state = THREAD_ZOMBIE;
    wait_queue_wake_all(&t->joiners);
    schedule();
    cpu_irq_restore(flags);
    for (;;)
        __asm__ volatile("cli; hlt");
}

void thread_exit(void)
{
    thread_kill_current(0);
}

int thread_join(thread *t)
{
    u64 flags;

    if (!t)
        return -1;
    if (t == g_sched.current)
        panic("sched: join self");
    flags = cpu_irq_save();
    while (t->state != THREAD_ZOMBIE)
        sched_block(&t->joiners);
    cpu_irq_restore(flags);
    thread_destroy(t);
    return 0;
}

i64 thread_join_code(thread *t)
{
    u64 flags;
    i64 code;

    if (!t)
        return -1;
    if (t == g_sched.current)
        panic("sched: join self");
    flags = cpu_irq_save();
    while (t->state != THREAD_ZOMBIE)
        sched_block(&t->joiners);
    cpu_irq_restore(flags);
    /* Read the code before thread_destroy frees the record. */
    code = t->exit_code;
    thread_destroy(t);
    return code;
}

thread *sched_current(void)
{
    return g_sched.current;
}

u64 sched_switches(void)
{
    return g_sched.switches;
}

u32 thread_id(thread *t)
{
    return t ? t->id : 0;
}

const char *thread_name(thread *t)
{
    return t ? t->name : "(null)";
}

static void idle_entry(void *arg)
{
    (void)arg;
    for (;;)
        __asm__ volatile("sti; hlt");
}

void sched_init(void)
{
    int c;

    memset(&g_sched, 0, sizeof(g_sched));
    memset(&g_boot_thread, 0, sizeof(g_boot_thread));
    for (c = 0; c < (int)SCHED_CLASSES; c++) {
        g_sched.rq[c].head = 0;
        g_sched.rq[c].tail = 0;
        g_sched.rq[c].count = 0;
    }
    g_sched.next_id = 1;
    g_boot_thread.id = 0;
    g_boot_thread.state = THREAD_RUNNING;
    g_boot_thread.class = SCHED_NORMAL;
    g_boot_thread.quantum = quantum_for(SCHED_NORMAL);
    memcpy(g_boot_thread.name, "boot", 5);
    g_boot_thread.stack_base = 0;
    wait_queue_init(&g_boot_thread.joiners);
    g_sched.current = &g_boot_thread;
    g_sched.idle = thread_create("idle", idle_entry, 0, SCHED_IDLE);
    if (!g_sched.idle)
        panic("sched: cannot create idle thread");
    if (cpu_self())
        cpu_self()->sched = &g_sched;
    kprintf("sched  : %s, boot thread id 0, idle id %u\n",
            "per-class run queues ready", g_sched.idle->id);
}

void sched_start(void)
{
    u64 flags;

    flags = cpu_irq_save();
    if (g_sched.started) {
        cpu_irq_restore(flags);
        return;
    }
    g_sched.started = 1;
    g_boot_thread.state = THREAD_RUNNABLE;
    g_boot_thread.quantum = quantum_for(SCHED_NORMAL);
    rq_push(&g_boot_thread);
    schedule();
    cpu_irq_restore(flags);
}

typedef struct worker_arg {
    volatile u64 counter;
    u64 iterations;
    u64 start_tick;
    u64 end_tick;
} worker_arg;

static void yield_worker(void *arg)
{
    worker_arg *wa = (worker_arg *)arg;
    u64 i;

    for (i = 0; i < wa->iterations; i++) {
        wa->counter++;
        sched_yield();
    }
}

static void preempt_worker(void *arg)
{
    worker_arg *wa = (worker_arg *)arg;
    u64 end = timer_ticks() + 3;

    while (timer_ticks() < end)
        wa->counter++;
}

static void sleep_worker(void *arg)
{
    worker_arg *wa = (worker_arg *)arg;

    wa->start_tick = timer_ticks();
    sched_sleep_ticks(2);
    wa->end_tick = timer_ticks();
    wa->counter = 1;
}

int sched_selftest(void)
{
    worker_arg a;
    worker_arg b;
    worker_arg s;
    thread *ta;
    thread *tb;
    thread *ts;
    u64 i;

    sched_start();

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.iterations = 50;
    b.iterations = 50;
    ta = thread_create("yield-a", yield_worker, &a, SCHED_NORMAL);
    tb = thread_create("yield-b", yield_worker, &b, SCHED_NORMAL);
    if (!ta || !tb)
        return 0;
    if (thread_join(ta) != 0 || thread_join(tb) != 0)
        return 0;
    if (a.counter != 50 || b.counter != 50)
        return 0;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    ta = thread_create("preempt-a", preempt_worker, &a, SCHED_NORMAL);
    tb = thread_create("preempt-b", preempt_worker, &b, SCHED_NORMAL);
    if (!ta || !tb)
        return 0;
    if (thread_join(ta) != 0 || thread_join(tb) != 0)
        return 0;
    if (a.counter == 0 || b.counter == 0)
        return 0;

    memset(&s, 0, sizeof(s));
    ts = thread_create("sleep", sleep_worker, &s, SCHED_NORMAL);
    if (!ts)
        return 0;
    if (thread_join(ts) != 0)
        return 0;
    if (s.counter != 1 || s.end_tick < s.start_tick + 2)
        return 0;

    for (i = 0; i < 100; i++)
        sched_yield();
    return 1;
}
