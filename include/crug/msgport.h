/* CrugOS typed, waitable message ports. */
#ifndef CRUG_MSGPORT_H
#define CRUG_MSGPORT_H

#include <crug/types.h>
#include <crug/thread.h>

#define MSGPORT_MAX_DEPTH 64u
#define MSGPORT_MAX_PAYLOAD 1024u

typedef struct msgport {
    struct message *head;
    struct message *tail;
    u32 count;
    u32 reserved;
    wait_queue receivers;
    wait_queue senders;
} msgport;

msgport *msgport_create(void);
void msgport_destroy(msgport *port);
int msgport_send(msgport *port, u64 type, const void *data, usize len);
i64 msgport_recv(msgport *port, u64 *type, void *data, usize cap);
u32 msgport_count(msgport *port);
int msgport_selftest(void);

#endif /* CRUG_MSGPORT_H */
