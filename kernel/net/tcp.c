/* CrugOS TCP: a small in-order transport for the socket self-tests.
 *
 * One static table of connection control blocks (no dynamic allocation),
 * a fixed 4 KiB receive window per socket, no options besides the implicit
 * MSS, and no timer-driven retransmission: an unacknowledged segment is
 * retransmitted by the blocking caller.  This is enough for the loopback
 * style HTTP client and server exchanges the boot self-test performs, and
 * it keeps the code small enough to reason about.
 */
#include <crug/net.h>
#include <crug/syscall.h>
#include <crug/kernel.h>
#include <crug/string.h>
#include <crug/cpu.h>
#include <crug/timer.h>

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

#define TCP_HEADER_BYTES 20u
#define TCP_EPHEMERAL_BASE 49152u
#define TCP_WAIT_ACK_MS 500u
#define TCP_HTTP_BUF_BYTES 512u

static const char g_http_get[] =
    "GET / HTTP/1.0\r\n"
    "Host: crugos\r\n"
    "\r\n";

static const char g_http_response[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Length: 14\r\n"
    "Connection: close\r\n"
    "\r\n"
    "crugos-served\n";

static struct net_tcp_socket g_sockets[NET_TCP_MAX];
static u32 g_peer_port;
static u32 g_serve_seconds;
static u32 g_isn;
/* Phase S1: half-open accounting.  The table holds four connections, so four
   silent SYNs used to deny service to every later one; an overflow is now
   answered with RST|ACK and stale half-open entries are reaped. */
static u32 g_syn_overflows;
static u32 g_syn_reaped;
static int g_syn_test_mode;
static int g_synflood_requested;

int tcp_reap_stale(u64 now_ns);
static u32 tcp_next_isn(void)
{
    g_isn += 0x9E3779B9u;
    return (u32)timer_ticks() ^ g_isn;
}

static int tcp_contains(const char *haystack, usize len, const char *needle)
{
    usize nlen = strlen(needle);
    usize i;

    if (nlen == 0 || nlen > len)
        return 0;
    for (i = 0; i + nlen <= len; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

void tcp_init(void)
{
    memset(g_sockets, 0, sizeof(g_sockets));
    g_peer_port = 0;
    g_serve_seconds = 0;
    g_isn = 0;
}

struct net_tcp_socket *tcp_socket_new(void)
{
    u32 i;

    (void)tcp_reap_stale(timer_monotonic_ns());
    for (i = 0; i < NET_TCP_MAX; i++) {
        struct net_tcp_socket *s = &g_sockets[i];

        if (s->used)
            continue;
        memset(s, 0, sizeof(*s));
        s->local_ip = net_config_get()->ip;
        s->local_port = (u16)(TCP_EPHEMERAL_BASE + i);
        s->used = 1;
        return s;
    }
    return 0;
}

void tcp_socket_release(struct net_tcp_socket *s)
{
    if (!s)
        return;
    s->state = NET_TCP_CLOSED;
    s->used = 0;
}

/* ---- Phase S1: half-open connection policy ------------------------------ */

int tcp_reap_stale(u64 now_ns)
{
    u32 i;
    int reaped = 0;

    for (i = 0; i < NET_TCP_MAX; i++) {
        struct net_tcp_socket *s = &g_sockets[i];

        if (!s->used)
            continue;
        if (s->state != NET_TCP_SYN_SENT && s->state != NET_TCP_SYN_RECEIVED)
            continue;
        if (now_ns <= s->state_ns + NET_TCP_SYN_TIMEOUT_NS)
            continue;
        s->used = 0;
        s->state = NET_TCP_CLOSED;
        g_syn_reaped++;
        reaped++;
    }
    return reaped;
}

static void tcp_send_reset(u32 dst_ip, u16 dst_port, u16 src_port, u32 seq,
                           u32 ack)
{
    u8 seg[TCP_HEADER_BYTES];

    if (g_syn_test_mode)
        return;             /* the selftest checks the decision, not the wire */
    memset(seg, 0, sizeof(seg));
    net_put16(seg, src_port);
    net_put16(seg + 2, dst_port);
    net_put32(seg + 4, seq);
    net_put32(seg + 8, ack);
    seg[12] = (u8)(5u << 4);
    seg[13] = (u8)(TCP_RST | TCP_ACK);
    net_put16(seg + 14, 0u);
    net_put16(seg + 16, net_tcp_checksum(net_config_get()->ip, dst_ip, seg,
                                         sizeof(seg)));
    (void)net_ipv4_send(dst_ip, NET_IP_PROTO_TCP, seg, sizeof(seg));
}

/* A reset is only honoured when its sequence number is inside our receive
   window (RFC 793 3.4): an off-path attacker who guesses the four-tuple but
   not the sequence must not be able to tear a connection down. */
static int tcp_rst_valid(const struct net_tcp_socket *s, u32 seq, u32 ack)
{
    u32 hi;

    if (s->state == NET_TCP_SYN_SENT)
        return ack == s->snd_nxt;
    hi = s->rcv_nxt + (u32)s->rx_count;
    if (hi == s->rcv_nxt)
        hi = s->rcv_nxt + 1u;
    if (!net_seq_le(s->rcv_nxt, seq) || !net_seq_lt(seq, hi))
        return 0;
    return 1;
}

static struct net_tcp_socket *tcp_lookup(u32 src_ip, u16 sport, u16 dport)
{
    u32 i;

    for (i = 0; i < NET_TCP_MAX; i++) {
        struct net_tcp_socket *s = &g_sockets[i];

        if (!s->used || s->state == NET_TCP_CLOSED || s->state == NET_TCP_LISTEN)
            continue;
        if (s->local_port == dport && s->remote_port == sport &&
            s->remote_ip == src_ip)
            return s;
    }
    for (i = 0; i < NET_TCP_MAX; i++) {
        struct net_tcp_socket *s = &g_sockets[i];

        if (s->used && s->state == NET_TCP_LISTEN && s->local_port == dport)
            return s;
    }
    return 0;
}

static int tcp_send_segment(struct net_tcp_socket *s, u8 flags, u32 seq, u32 ack,
                            const void *payload, usize len)
{
    u8 seg[TCP_HEADER_BYTES + NET_TCP_MSS];
    u32 free_bytes = s->rx_count < NET_TCP_RX_BYTES ? NET_TCP_RX_BYTES - s->rx_count : 0;

    if (!s || len > NET_TCP_MSS)
        return -1;
    memset(seg, 0, TCP_HEADER_BYTES);
    net_put16(seg, s->local_port);
    net_put16(seg + 2, s->remote_port);
    net_put32(seg + 4, seq);
    net_put32(seg + 8, ack);
    seg[12] = (u8)(5u << 4);
    seg[13] = flags;
    net_put16(seg + 14, (u16)free_bytes);
    net_put16(seg + 18, 0);
    if (len && payload)
        memcpy(seg + TCP_HEADER_BYTES, payload, len);
    net_put16(seg + 16, net_tcp_checksum(s->local_ip, s->remote_ip, seg,
                                         TCP_HEADER_BYTES + len));
    return net_ipv4_send(s->remote_ip, NET_IP_PROTO_TCP, seg, TCP_HEADER_BYTES + len);
}

static void tcp_enqueue(struct net_tcp_socket *s, const u8 *data, usize len)
{
    usize i;

    for (i = 0; i < len && s->rx_count < NET_TCP_RX_BYTES; i++) {
        u32 tail = (s->rx_head + s->rx_count) % NET_TCP_RX_BYTES;

        s->rx[tail] = data[i];
        s->rx_count++;
    }
}

static void tcp_listen_input(struct net_tcp_socket *ls, u32 src_ip, u16 sport,
                             u32 seq, u8 flags)
{
    struct net_tcp_socket *s;

    if (!(flags & TCP_SYN) || (flags & TCP_ACK))
        return;
    s = tcp_socket_new();
    if (!s) {
        /* Phase S1: refuse the handshake instead of dropping it silently.  A
           SYN that gets no answer leaves the peer retrying while our table
           stays exhausted; RST|ACK fails the handshake fast. */
        g_syn_overflows++;
        tcp_send_reset(src_ip, ls->local_port, sport, 0u, seq + 1u);
        return;
    }
    s->state = NET_TCP_SYN_RECEIVED;
    s->state_ns = timer_monotonic_ns();
    s->remote_ip = src_ip;
    s->remote_port = sport;
    s->local_port = ls->local_port;
    s->listen_port = ls->local_port;
    s->rcv_nxt = seq + 1;
    s->snd_una = tcp_next_isn();
    s->snd_nxt = s->snd_una + 1;
    tcp_send_segment(s, (u8)(TCP_SYN | TCP_ACK), s->snd_una, s->rcv_nxt, 0, 0);
}

void tcp_receive_segment(u32 src_ip, u32 dst_ip, const u8 *seg, usize len)
{
    struct net_tcp_socket *s;
    usize hlen;
    u16 sport;
    u16 dport;
    u32 seq;
    u32 ack;
    u8 flags;
    usize plen;

    (void)dst_ip;
    if (len < TCP_HEADER_BYTES)
        return;
    hlen = (usize)(seg[12] >> 4) * 4;
    if (hlen < TCP_HEADER_BYTES || hlen > len)
        return;
    if (!net_tcp_checksum_ok(src_ip, dst_ip, seg, len))
        return;
    sport = net_get16(seg);
    dport = net_get16(seg + 2);
    seq = net_get32(seg + 4);
    ack = net_get32(seg + 8);
    flags = seg[13];
    plen = len - hlen;
    s = tcp_lookup(src_ip, sport, dport);
    if (!s)
        return;
    if (flags & TCP_RST) {
        if (!tcp_rst_valid(s, seq, (flags & TCP_ACK) ? ack : 0u))
            return;
        s->state = NET_TCP_CLOSED;
        s->peer_closed = 1;
        return;
    }
    if (s->state == NET_TCP_LISTEN) {
        tcp_listen_input(s, src_ip, sport, seq, flags);
        return;
    }
    if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
        s->state == NET_TCP_SYN_SENT) {
        s->snd_una = ack;
        s->snd_wnd = net_get16(seg + 14);
        s->rcv_nxt = seq + 1;
        if (ack == s->snd_nxt) {
            s->state = NET_TCP_ESTABLISHED;
            tcp_send_segment(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, 0, 0);
        }
        return;
    }
    if ((flags & TCP_ACK) && s->state == NET_TCP_SYN_RECEIVED &&
        ack == s->snd_nxt) {
        s->snd_una = ack;
        s->state = NET_TCP_ESTABLISHED;
    }
    if ((flags & TCP_ACK) && s->state == NET_TCP_ESTABLISHED &&
        net_seq_gt(ack, s->snd_una) && net_seq_le(ack, s->snd_nxt))
        s->snd_una = ack;
    if (plen) {
        if (seq == s->rcv_nxt) {
            tcp_enqueue(s, seg + hlen, plen);
            s->rcv_nxt += (u32)plen;
            tcp_send_segment(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, 0, 0);
        } else if (s->state != NET_TCP_CLOSED && s->state != NET_TCP_TIME_WAIT) {
            tcp_send_segment(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, 0, 0);
        }
    }
    if (flags & TCP_FIN) {
        s->rcv_nxt += 1;
        s->peer_closed = 1;
        tcp_send_segment(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, 0, 0);
        if (s->state == NET_TCP_ESTABLISHED)
            s->state = NET_TCP_CLOSE_WAIT;
        else if (s->state == NET_TCP_FIN_WAIT_1 || s->state == NET_TCP_FIN_WAIT_2)
            s->state = NET_TCP_TIME_WAIT;
    }
}

int tcp_synflood_requested(void)
{
    return g_synflood_requested;
}

/* Phase S1 gate 68: fill the table, feed one more SYN, then prove the four
   stale half-open entries come back.  The wire path is skipped so the test
   needs no device, no ARP and no peer. */
int tcp_synflood_selftest(void)
{
    struct net_tcp_socket *ls;
    struct net_tcp_socket *a;
    struct net_tcp_socket *b;
    struct net_tcp_socket *c;
    struct net_tcp_socket *fresh;
    u8 syn[TCP_HEADER_BYTES];
    u64 now;
    u32 rsts;
    int ok = 1;

    tcp_init();
    g_syn_test_mode = 1;
    rsts = g_syn_overflows;
    ls = tcp_socket_new();
    if (!ls || tcp_listen(ls, (u16)NET_TCP_DEFAULT_HTTP_PORT) != 0)
        ok = 0;
    a = tcp_socket_new();
    b = tcp_socket_new();
    c = tcp_socket_new();
    if (!a || !b || !c)
        ok = 0;
    if (ok) {
        memset(syn, 0, sizeof(syn));
        net_put16(syn, 40000u);
        net_put16(syn + 2, (u16)NET_TCP_DEFAULT_HTTP_PORT);
        net_put32(syn + 4, 1000u);
        syn[12] = (u8)(5u << 4);
        syn[13] = TCP_SYN;
        net_put16(syn + 16, net_tcp_checksum(0x0A000263u,
                                             net_config_get()->ip, syn,
                                             sizeof(syn)));
        tcp_receive_segment(0x0A000263u, net_config_get()->ip, syn,
                            sizeof(syn));
        if (g_syn_overflows != rsts + 1u)
            ok = 0;
    }
    if (ok) {
        /* Absolute time is tiny this early in boot, so backdate by starting the
           stamp at zero instead of subtracting from now. */
        now = NET_TCP_SYN_TIMEOUT_NS + 1ull;
        a->state = NET_TCP_SYN_RECEIVED;
        a->state_ns = 0;
        b->state = NET_TCP_SYN_RECEIVED;
        b->state_ns = 0;
        c->state = NET_TCP_SYN_SENT;
        c->state_ns = 0;
        if (tcp_reap_stale(now) != 3)
            ok = 0;
        fresh = tcp_socket_new();
        if (!fresh)
            ok = 0;
        else
            tcp_socket_release(fresh);
    }
    g_syn_test_mode = 0;
    tcp_init();
    if (!ok) {
        kprintf("CRUGOS: tcp syn flood selftest failed\n");
        return 0;
    }
    kprintf("CRUGOS: tcp syn flood selftest ok (%u overflow(s), %u reaped)\n",
            (unsigned)g_syn_overflows, (unsigned)g_syn_reaped);
    return 1;
}

static int tcp_wait_ack(struct net_tcp_socket *s, u32 want, u32 timeout_ms)
{
    u64 deadline = timer_monotonic_ns() + (u64)timeout_ms * 1000000ull;

    while (net_seq_lt(s->snd_una, want)) {
        if (s->peer_closed || s->state == NET_TCP_CLOSED)
            return -1;
        if (timer_monotonic_ns() >= deadline)
            return -1;
        net_poll();
        cpu_wait_for_irq();
    }
    return 0;
}

int tcp_connect(struct net_tcp_socket *s, u32 ip, u16 port, u32 timeout_ms)
{
    u32 attempt;
    u32 isn;

    if (!s)
        return -1;
    s->remote_ip = ip;
    s->remote_port = port;
    s->state = NET_TCP_SYN_SENT;
    s->state_ns = timer_monotonic_ns();
    s->peer_closed = 0;
    s->rx_head = 0;
    s->rx_count = 0;
    isn = tcp_next_isn();
    s->snd_una = isn;
    s->snd_nxt = isn + 1;
    s->rcv_nxt = 0;
    for (attempt = 0; attempt < NET_TCP_RETRIES; attempt++) {
        u64 deadline = timer_monotonic_ns() + (u64)timeout_ms * 1000000ull;

        tcp_send_segment(s, TCP_SYN, isn, 0, 0, 0);
        while (timer_monotonic_ns() < deadline) {
            net_poll();
            if (s->state == NET_TCP_ESTABLISHED)
                return 0;
            if (s->state == NET_TCP_CLOSED)
                return -1;
            cpu_wait_for_irq();
        }
    }
    return -1;
}

int tcp_listen(struct net_tcp_socket *s, u16 port)
{
    if (!s || port == 0)
        return -1;
    s->state = NET_TCP_LISTEN;
    s->local_port = port;
    s->listen_port = port;
    return 0;
}

struct net_tcp_socket *tcp_accept(struct net_tcp_socket *ls, u32 timeout_ms)
{
    u64 deadline;

    if (!ls || ls->state != NET_TCP_LISTEN)
        return 0;
    deadline = timer_monotonic_ns() + (u64)timeout_ms * 1000000ull;
    for (;;) {
        u32 i;

        for (i = 0; i < NET_TCP_MAX; i++) {
            struct net_tcp_socket *s = &g_sockets[i];

            if (s->used && !s->accepted && s->state == NET_TCP_ESTABLISHED &&
                s->listen_port == ls->local_port) {
                s->accepted = 1;
                return s;
            }
        }
        net_poll();
        if (timer_monotonic_ns() >= deadline)
            return 0;
        cpu_wait_for_irq();
    }
}

int tcp_send(struct net_tcp_socket *s, const void *data, usize len)
{
    const u8 *p = (const u8 *)data;
    usize off = 0;

    if (!s || (!data && len))
        return -1;
    while (off < len) {
        usize chunk = len - off;
        u32 seq;
        u32 attempt;

        if (chunk > NET_TCP_MSS)
            chunk = NET_TCP_MSS;
        seq = s->snd_nxt;
        /* Advance snd_nxt with the send: the peer acknowledges seq + chunk,
           which must stay inside the [snd_una, snd_nxt] window. */
        s->snd_nxt = seq + (u32)chunk;
        for (attempt = 0; attempt < NET_TCP_RETRIES; attempt++) {
            if (tcp_send_segment(s, (u8)(TCP_ACK | TCP_PSH), seq, s->rcv_nxt,
                                 p + off, chunk) != 0)
                return -1;
            if (tcp_wait_ack(s, seq + (u32)chunk, TCP_WAIT_ACK_MS) == 0)
                break;
            if (s->peer_closed || s->state == NET_TCP_CLOSED)
                return -1;
        }
        if (attempt == NET_TCP_RETRIES)
            return -1;
        off += chunk;
    }
    return (int)len;
}

int tcp_recv(struct net_tcp_socket *s, void *data, usize len, u32 timeout_ms)
{
    u8 *out = (u8 *)data;
    usize n = 0;

    if (!s || (!data && len) || len == 0)
        return -1;
    if (s->rx_count == 0 && !s->peer_closed) {
        u64 deadline = timer_monotonic_ns() + (u64)timeout_ms * 1000000ull;

        while (s->rx_count == 0) {
            net_poll();
            if (s->rx_count)
                break;
            if (s->peer_closed || s->state == NET_TCP_CLOSED)
                break;
            if (timer_monotonic_ns() >= deadline)
                return -1;
            cpu_wait_for_irq();
        }
    }
    while (n < len && s->rx_count > 0) {
        out[n++] = s->rx[s->rx_head];
        s->rx_head = (s->rx_head + 1) % NET_TCP_RX_BYTES;
        s->rx_count--;
    }
    return (int)n;
}

int tcp_close(struct net_tcp_socket *s, u32 timeout_ms)
{
    (void)timeout_ms;
    if (!s)
        return -1;
    if (s->state == NET_TCP_ESTABLISHED || s->state == NET_TCP_CLOSE_WAIT) {
        tcp_send_segment(s, (u8)(TCP_FIN | TCP_ACK), s->snd_nxt, s->rcv_nxt, 0, 0);
        s->snd_nxt += 1;
    }
    s->state = NET_TCP_CLOSED;
    return 0;
}

static void tcp_cmdline_token(const char *tok, usize len)
{
    static const char peer_prefix[] = "net.tcp=";
    static const char serve_prefix[] = "net.serve=";
    static const char flood_prefix[] = "net.synflood=";
    usize k;
    u32 v;

    if (len > sizeof(peer_prefix) - 1 &&
        memcmp(tok, peer_prefix, sizeof(peer_prefix) - 1) == 0) {
        v = 0;
        for (k = sizeof(peer_prefix) - 1; k < len; k++) {
            if (tok[k] < '0' || tok[k] > '9')
                return;
            v = v * 10u + (u32)(tok[k] - '0');
        }
        if (v >= 1 && v <= 65535)
            g_peer_port = v;
        return;
    }
    if (len > sizeof(serve_prefix) - 1 &&
        memcmp(tok, serve_prefix, sizeof(serve_prefix) - 1) == 0) {
        v = 0;
        for (k = sizeof(serve_prefix) - 1; k < len; k++) {
            if (tok[k] < '0' || tok[k] > '9')
                return;
            v = v * 10u + (u32)(tok[k] - '0');
        }
        if (v >= 1 && v <= 3600)
            g_serve_seconds = v;
        return;
    }
    if (len > sizeof(flood_prefix) - 1 &&
        memcmp(tok, flood_prefix, sizeof(flood_prefix) - 1) == 0) {
        if (len - (sizeof(flood_prefix) - 1) == 1 &&
            tok[sizeof(flood_prefix) - 1] == '1')
            g_synflood_requested = 1;
    }
}

void tcp_cmdline(const boot_info *bi)
{
    const char *line;
    usize len;
    usize i;

    if (!bi || !bi->command_line || bi->command_line_len == 0)
        return;
    line = bi->command_line;
    len = (usize)bi->command_line_len;
    i = 0;
    while (i < len) {
        usize start;

        while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\n' ||
                           line[i] == '\r'))
            i++;
        start = i;
        while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '\n' &&
               line[i] != '\r')
            i++;
        if (i > start)
            tcp_cmdline_token(line + start, i - start);
    }
}

int tcp_selftest(void)
{
    struct net_tcp_socket *ls;
    struct net_tcp_socket *cl;
    struct net_tcp_socket *peer;
    char buf[TCP_HTTP_BUF_BYTES];
    int n;

    if (!g_peer_port || net_device_count() == 0) {
        kprintf("CRUGOS: tcp selftest skipped\n");
        return 1;
    }
    ls = tcp_socket_new();
    if (!ls || tcp_listen(ls, (u16)NET_TCP_DEFAULT_HTTP_PORT) != 0) {
        kprintf("CRUGOS: net selftest failed (tcp listen)\n");
        return 0;
    }
    kprintf("CRUGOS: tcp listen ready (port %u)\n",
            (unsigned)NET_TCP_DEFAULT_HTTP_PORT);
    cl = tcp_socket_new();
    if (!cl || tcp_connect(cl, net_config_get()->gateway, (u16)g_peer_port, 3000u) != 0) {
        kprintf("CRUGOS: net selftest failed (tcp connect)\n");
        return 0;
    }
    kprintf("CRUGOS: tcp connect ok (%u.%u.%u.%u:%u)\n",
            (unsigned)((net_config_get()->gateway >> 24) & 0xFFu),
            (unsigned)((net_config_get()->gateway >> 16) & 0xFFu),
            (unsigned)((net_config_get()->gateway >> 8) & 0xFFu),
            (unsigned)(net_config_get()->gateway & 0xFFu),
            (unsigned)g_peer_port);
    if (tcp_send(cl, g_http_get, sizeof(g_http_get) - 1) < 0) {
        kprintf("CRUGOS: net selftest failed (tcp http)\n");
        return 0;
    }
    n = tcp_recv(cl, buf, sizeof(buf), 3000u);
    if (n <= 0 || !tcp_contains(buf, (usize)n, "200 OK") ||
        !tcp_contains(buf, (usize)n, "crugos-tcp-ok")) {
        kprintf("CRUGOS: net selftest failed (tcp http)\n");
        return 0;
    }
    kprintf("CRUGOS: tcp http ok (%d bytes)\n", n);
    tcp_close(cl, 1000u);
    tcp_socket_release(cl);
    if (syscall_socket_selftest(net_config_get()->gateway, (u16)g_peer_port) != 0) {
        kprintf("CRUGOS: net selftest failed (tcp socket)\n");
        return 0;
    }
    kprintf("CRUGOS: tcp socket syscall ok\n");
    peer = tcp_accept(ls, g_serve_seconds * 1000u);
    if (!peer) {
        kprintf("CRUGOS: net selftest failed (tcp serve)\n");
        return 0;
    }
    n = tcp_recv(peer, buf, sizeof(buf), 3000u);
    if (n <= 0) {
        kprintf("CRUGOS: net selftest failed (tcp serve)\n");
        return 0;
    }
    if (tcp_send(peer, g_http_response, sizeof(g_http_response) - 1) < 0) {
        kprintf("CRUGOS: net selftest failed (tcp serve)\n");
        return 0;
    }
    kprintf("CRUGOS: tcp http served (%u bytes)\n",
            (unsigned)(sizeof(g_http_response) - 1));
    tcp_close(peer, 1000u);
    tcp_socket_release(peer);
    tcp_close(ls, 1000u);
    tcp_socket_release(ls);
    return 1;
}
