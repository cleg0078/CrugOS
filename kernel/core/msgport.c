/* CrugOS typed message ports: O(1) enqueue/dequeue and blocking wait. */
#include <crug/msgport.h>
#include <crug/harden.h>
#include <crug/heap.h>
#include <crug/cpu.h>
#include <crug/string.h>
#include <crug/kernel.h>

struct message {
    struct message *next;
    u64 type;
    u64 len;
    u8 payload[];
};

msgport *msgport_create(void)
{
    msgport *p = (msgport *)kzalloc(sizeof(msgport));

    if (!p)
        return 0;
    wait_queue_init(&p->receivers);
    wait_queue_init(&p->senders);
    return p;
}

void msgport_destroy(msgport *port)
{
    struct message *m;
    u64 flags;

    if (!port)
        return;
    flags = cpu_irq_save();
    m = port->head;
    port->head = 0;
    port->tail = 0;
    port->count = 0;
    cpu_irq_restore(flags);
    while (m) {
        struct message *next = m->next;

        kfree(m);
        m = next;
    }
    kfree(port);
}

int msgport_send(msgport *port, u64 type, const void *data, usize len)
{
    struct message *m;
    u64 flags;

    if (!port || len > MSGPORT_MAX_PAYLOAD)
        return -1;
    if (!harden_payload_ok(data, (u64)len))
        return -1;
    m = (struct message *)kmalloc(sizeof(struct message) + len);
    if (!m)
        return -1;
    m->next = 0;
    m->type = type;
    m->len = len;
    if (len && data)
        memcpy(m->payload, data, len);

    flags = cpu_irq_save();
    while (port->count >= MSGPORT_MAX_DEPTH)
        sched_block(&port->senders);
    if (port->tail)
        port->tail->next = m;
    else
        port->head = m;
    port->tail = m;
    port->count++;
    wait_queue_wake_one(&port->receivers);
    cpu_irq_restore(flags);
    return 0;
}

i64 msgport_recv(msgport *port, u64 *type, void *data, usize cap)
{
    struct message *m;
    u64 flags;
    usize n;

    if (!port)
        return -1;
    flags = cpu_irq_save();
    while (port->count == 0)
        sched_block(&port->receivers);
    m = port->head;
    port->head = m->next;
    if (!port->head)
        port->tail = 0;
    port->count--;
    wait_queue_wake_one(&port->senders);
    cpu_irq_restore(flags);

    if (type)
        *type = m->type;
    n = m->len < cap ? (usize)m->len : cap;
    if (n && data)
        memcpy(data, m->payload, n);
    /* Phase S1: report what was actually copied, not what the sender wrote.
       Returning the full length told a caller with a small buffer that its
       tail had been filled when it never was. */
    kfree(m);
    return (i64)n;
}

u32 msgport_count(msgport *port)
{
    u64 flags;
    u32 n;

    if (!port)
        return 0;
    flags = cpu_irq_save();
    n = port->count;
    cpu_irq_restore(flags);
    return n;
}

typedef struct ipc_worker {
    msgport *port;
    u64 tag;
} ipc_worker;

static void ipc_sender(void *arg)
{
    ipc_worker *w = (ipc_worker *)arg;
    u64 i;

    for (i = 0; i < 4; i++)
        msgport_send(w->port, w->tag + i, &i, sizeof(i));
}

int msgport_selftest(void)
{
    msgport *p = msgport_create();
    ipc_worker w;
    thread *t;
    u64 type;
    u64 value;
    u64 i;

    if (!p)
        return 0;
    w.port = p;
    w.tag = 0x100;
    t = thread_create("ipc-send", ipc_sender, &w, SCHED_NORMAL);
    if (!t) {
        msgport_destroy(p);
        return 0;
    }
    for (i = 0; i < 4; i++) {
        i64 n = msgport_recv(p, &type, &value, sizeof(value));

        if (n != (i64)sizeof(value))
            return 0;
        if (type != 0x100 + i || value != i)
            return 0;
    }
    if (thread_join(t) != 0)
        return 0;
    if (msgport_count(p) != 0)
        return 0;
    msgport_destroy(p);
    return 1;
}
