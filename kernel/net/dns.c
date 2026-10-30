/* net.dns: a small caching DNS stub resolver (phase 4D).
 *
 * A query goes to the server from the DHCP lease (or net.dns=) over UDP; a
 * timeout, a truncated reply or net.dns-tcp=1 sends it over TCP instead.
 * Answers are cached in a fixed table until their TTL expires. The parser
 * only accepts a reply that matches the transaction id and stays inside the
 * received length, and every name it walks is bounded, so a malformed or
 * hostile packet cannot run the resolver off the end of its buffers.
 */
#include <crug/net.h>
#include <crug/kernel.h>
#include <crug/string.h>
#include <crug/timer.h>
#include <crug/cpu.h>
#include <crug/syscall.h>

#define DNS_FLAG_RESPONSE 0x8000u
#define DNS_FLAG_TRUNCATED 0x0200u
#define DNS_FLAG_RCODE 0x000Fu
#define DNS_QTYPE_A 1u
#define DNS_QTYPE_CNAME 5u
#define DNS_QCLASS_IN 1u
#define DNS_HEADER_BYTES 12u
#define DNS_MAX_LABEL 63u
#define DNS_MAX_NAME_BYTES 253u
#define DNS_MAX_HOPS 16u
#define DNS_TTL_MAX 86400u
#define DNS_ARP_TIMEOUT_MS 3000u
/* Phase S1: room for a long backwards pointer chain in the hostile selftest. */
#define DNS_HOSTILE_BYTES 4096u

/* Idling uses sti; hlt; cli, which is privileged: the host unit tests compile
 * this file with CRUG_HOST_TEST and let net_poll() advance a fake clock. */
#ifdef CRUG_HOST_TEST
#define dns_idle() ((void)0)
#else
#define dns_idle() cpu_wait_for_irq()
#endif

struct dns_cache_entry {
    char name[NET_DNS_NAME_MAX];
    u32 addr[NET_DNS_MAX_ADDRS];
    u32 count;
    u32 ttl;
    u64 expires_ns;
    int used;
};

static struct dns_cache_entry g_cache[NET_DNS_CACHE];
static u32 g_server;
static u16 g_server_port = (u16)NET_DNS_PORT;
static int g_force_tcp;
static char g_name[NET_DNS_NAME_MAX];
static const char *g_transport = "udp";
static u8 g_rx[NET_DNS_MSG_MAX];
static volatile u32 g_rx_len;
static u16 g_rx_id;
/* Phase S1: a reply is only accepted when it comes from the server we asked
 * and arrives on the port we sent it from.  Anything else arriving on our
 * ports is an off-path forgery attempt, not an answer. */
static u32 g_query_server;
static u16 g_local_port = (u16)NET_DNS_LOCAL_PORT;
/* Phase S1: the transaction id comes from this xorshift64 stream, seeded
 * from the loader entropy and the monotonic clock, instead of a counter. */
static u64 g_rng = 0x9E3779B97F4A7C15ull;
static int g_hostile_test;

static void dns_udp_handler(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port,
                            const void *data, usize len);

static usize dns_strlen(const char *s)
{
    usize n = 0;

    if (!s)
        return 0;
    while (s[n])
        n++;
    return n;
}

/* Lowercase and strip one trailing dot so `A.Example.` and `a.example` share
 * a cache entry. */
static void dns_key(const char *name, char *key, usize cap)
{
    usize n, i;

    if (!key || !cap)
        return;
    n = dns_strlen(name);
    while (n > 0 && name[n - 1] == '.')
        n--;
    if (n >= cap)
        n = cap - 1u;
    for (i = 0; i < n; i++) {
        char c = name[i];

        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        key[i] = c;
    }
    key[n] = 0;
}

/* out must have room for 16 bytes. */
static void dns_put_ip(char *out, usize cap, u32 ip)
{
    static const u32 divs[4] = { 16777216u, 65536u, 256u, 1u };
    usize o = 0;
    u32 k;

    if (!out || cap < 16u)
        return;
    for (k = 0; k < 4u; k++) {
        u32 v = (ip / divs[k]) % 256u;

        if (v >= 100u)
            out[o++] = (char)('0' + (v / 100u));
        if (v >= 10u)
            out[o++] = (char)('0' + ((v / 10u) % 10u));
        out[o++] = (char)('0' + (v % 10u));
        if (k < 3u)
            out[o++] = '.';
    }
    out[o] = 0;
}

static int dns_cache_lookup(const char *name, struct net_dns_answer *answer)
{
    char key[NET_DNS_NAME_MAX];
    u64 now = timer_monotonic_ns();
    u32 i, k;

    if (!name || !answer)
        return -1;
    dns_key(name, key, sizeof(key));
    if (!key[0])
        return -1;
    for (i = 0; i < NET_DNS_CACHE; i++) {
        if (!g_cache[i].used)
            continue;
        if (strcmp(g_cache[i].name, key) != 0)
            continue;
        if (now >= g_cache[i].expires_ns) {
            g_cache[i].used = 0;
            continue;
        }
        answer->count = g_cache[i].count;
        answer->ttl = g_cache[i].ttl;
        for (k = 0; k < g_cache[i].count && k < NET_DNS_MAX_ADDRS; k++)
            answer->addr[k] = g_cache[i].addr[k];
        return 0;
    }
    return -1;
}

static void dns_cache_store(const char *name, const struct net_dns_answer *answer)
{
    char key[NET_DNS_NAME_MAX];
    u64 now = timer_monotonic_ns();
    u64 oldest;
    u32 ttl, i, slot = (u32)NET_DNS_CACHE;

    if (!name || !answer || answer->count == 0u)
        return;
    ttl = answer->ttl;
    if (ttl == 0u)
        return;
    if (ttl > DNS_TTL_MAX)
        ttl = DNS_TTL_MAX;
    dns_key(name, key, sizeof(key));
    if (!key[0])
        return;
    for (i = 0; i < NET_DNS_CACHE; i++) {
        if (g_cache[i].used && strcmp(g_cache[i].name, key) == 0) {
            slot = i;
            break;
        }
    }
    if (slot == (u32)NET_DNS_CACHE) {
        for (i = 0; i < NET_DNS_CACHE; i++) {
            if (!g_cache[i].used) {
                slot = i;
                break;
            }
        }
    }
    if (slot == (u32)NET_DNS_CACHE) {
        oldest = ~(u64)0;
        slot = 0;
        for (i = 0; i < NET_DNS_CACHE; i++) {
            if (g_cache[i].expires_ns < oldest) {
                oldest = g_cache[i].expires_ns;
                slot = i;
            }
        }
    }
    g_cache[slot].used = 1;
    g_cache[slot].ttl = ttl;
    g_cache[slot].expires_ns = now + (u64)ttl * 1000000000ull;
    g_cache[slot].count = answer->count > NET_DNS_MAX_ADDRS ? NET_DNS_MAX_ADDRS : answer->count;
    for (i = 0; i < g_cache[slot].count; i++)
        g_cache[slot].addr[i] = answer->addr[i];
    strncpy(g_cache[slot].name, key, sizeof(g_cache[slot].name) - 1u);
    g_cache[slot].name[sizeof(g_cache[slot].name) - 1u] = 0;
}

usize dns_build_query(const char *name, u16 id, u8 *out, usize cap)
{
    usize i = 0, total = 0, q;

    if (!name || !out || !name[0] || cap < DNS_HEADER_BYTES + 5u)
        return 0;
    memset(out, 0, DNS_HEADER_BYTES);
    net_put16(out, id);
    net_put16(out + 2u, 0x0100u); /* recursion desired */
    net_put16(out + 4u, 1u);
    while (name[i]) {
        usize start = i, lab;

        while (name[i] && name[i] != '.')
            i++;
        lab = i - start;
        if (lab == 0 || lab > DNS_MAX_LABEL)
            return 0;
        if (total + 1u + lab > DNS_MAX_NAME_BYTES)
            return 0;
        if (DNS_HEADER_BYTES + total + 1u + lab + 5u > cap)
            return 0;
        out[DNS_HEADER_BYTES + total] = (u8)lab;
        memcpy(out + DNS_HEADER_BYTES + total + 1u, name + start, lab);
        total += 1u + lab;
        if (name[i] == '.')
            i++;
    }
    if (total == 0)
        return 0;
    out[DNS_HEADER_BYTES + total] = 0;
    q = DNS_HEADER_BYTES + total + 1u;
    net_put16(out + q, DNS_QTYPE_A);
    net_put16(out + q + 2u, DNS_QCLASS_IN);
    return q + 4u;
}

static usize dns_skip_name(const u8 *msg, usize len, usize pos)
{
    usize p = pos;
    usize next = 0;
    u32 hops = 0;

    for (;;) {
        u8 l;

        if (p >= len)
            return 0;
        l = msg[p];
        if (l == 0)
            return next ? next : p + 1u;
        if ((l & 0xC0u) == 0xC0u) {
            u16 ptr;

            if (p + 1u >= len)
                return 0;
            ptr = (u16)(((u16)(l & 0x3Fu) << 8) | msg[p + 1u]);
            if ((usize)ptr >= p)
                return 0; /* pointers must walk backwards, so this ends */
            /* A pointer ends the field it appears in: the next record
             * starts after the two pointer bytes, not after the target. */
            if (!next)
                next = p + 2u;
            if (++hops > DNS_MAX_HOPS)
                return 0;
            p = (usize)ptr;
            continue;
        }
        if (l > DNS_MAX_LABEL)
            return 0;
        if (p + 1u + (usize)l > len)
            return 0;
        p += 1u + (usize)l;
    }
}

/* Decode one possibly compressed name into out.
 *
 * Phase S1: this used to recurse once per compression pointer, and its hop
 * budget was a local variable, so every frame started counting from zero and
 * the budget never limited the depth.  A reply carrying a chain of backwards
 * pointers therefore turned the 16 KiB kernel stack into a function of the
 * packet.  The walk is one flat loop now, exactly like dns_skip_name: a
 * pointer only moves p backwards, every jump spends one hop from a single
 * budget, and next remembers the end of the first field. */
usize dns_decode_name(const u8 *msg, usize len, usize pos, char *out, usize cap)
{
    usize p = pos, o = 0, next = 0;
    u32 hops = 0;

    if (!msg || !out || cap == 0u)
        return 0;
    for (;;) {
        u8 l;

        if (p >= len)
            return 0;
        l = msg[p];
        if (l == 0) {
            out[o] = 0;
            return next ? next : p + 1u;
        }
        if ((l & 0xC0u) == 0xC0u) {
            u16 ptr;

            if (p + 1u >= len)
                return 0;
            ptr = (u16)(((u16)(l & 0x3Fu) << 8) | msg[p + 1u]);
            if ((usize)ptr >= p)
                return 0; /* backwards only, which also rules out cycles */
            if (!next)
                next = p + 2u;
            if (++hops > DNS_MAX_HOPS)
                return 0;
            p = (usize)ptr;
            continue;
        }
        if (l > DNS_MAX_LABEL)
            return 0;
        if (p + 1u + (usize)l > len)
            return 0;
        if (o && o + 1u >= cap)
            return 0;
        if (o)
            out[o++] = '.';
        if (o + (usize)l >= cap)
            return 0;
        memcpy(out + o, msg + p + 1u, (usize)l);
        o += (usize)l;
        p += 1u + (usize)l;
    }
}

int dns_parse_response(const u8 *msg, usize len, u16 id,
                       struct net_dns_answer *answer, char *cname, usize cname_cap)
{
    u16 flags, questions, records;
    usize p;
    u32 i;

    if (!msg || !answer || len < DNS_HEADER_BYTES)
        return -1;
    if (net_get16(msg) != id)
        return -1;
    flags = net_get16(msg + 2u);
    if (!(flags & DNS_FLAG_RESPONSE))
        return -1;
    if ((flags & DNS_FLAG_RCODE) != 0u)
        return -1;
    answer->count = 0;
    answer->ttl = 0;
    if (cname && cname_cap)
        cname[0] = 0;
    if (flags & DNS_FLAG_TRUNCATED)
        return -2;
    questions = net_get16(msg + 4u);
    records = net_get16(msg + 6u);
    if (questions != 1u || records == 0u)
        return -1;
    p = dns_skip_name(msg, len, DNS_HEADER_BYTES);
    if (!p || p + 4u > len)
        return -1;
    if (net_get16(msg + p) != DNS_QTYPE_A || net_get16(msg + p + 2u) != DNS_QCLASS_IN)
        return -1;
    p += 4u;
    for (i = 0; i < records; i++) {
        u16 type, cls, rdlen;
        u32 ttl;

        p = dns_skip_name(msg, len, p);
        if (!p || p + 10u > len)
            return -1;
        type = net_get16(msg + p);
        cls = net_get16(msg + p + 2u);
        ttl = net_get32(msg + p + 4u);
        rdlen = net_get16(msg + p + 8u);
        p += 10u;
        if (p + (usize)rdlen > len)
            return -1;
        if (cls == DNS_QCLASS_IN && type == DNS_QTYPE_A && rdlen == 4u) {
            if (answer->count < NET_DNS_MAX_ADDRS) {
                answer->addr[answer->count] = net_get32(msg + p);
                answer->count++;
                if (answer->ttl == 0u || ttl < answer->ttl)
                    answer->ttl = ttl;
            }
        } else if (cls == DNS_QCLASS_IN && type == DNS_QTYPE_CNAME && rdlen != 0u &&
                   cname && cname_cap && !cname[0]) {
            if (!dns_decode_name(msg, len, p, cname, cname_cap))
                return -1;
        }
        p += (usize)rdlen;
    }
    if (answer->count == 0u && (!cname || !cname[0]))
        return -1;
    return 0;
}

static void dns_udp_handler(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port,
                            const void *data, usize len)
{
    const u8 *msg = (const u8 *)data;

    (void)dst_ip;
    if (!msg || len < DNS_HEADER_BYTES || len > NET_DNS_MSG_MAX)
        return;
    /* Phase S1: the reply used to be accepted from any address and any
     * source port on a fixed local port, so guessing the 16-bit id was
     * enough.  Now the source address, the source port and the destination
     * port all have to match the query in flight. */
    if (!g_query_server || src_ip != g_query_server || src_port != g_server_port)
        return;
    if (dst_port != g_local_port)
        return;
    if (net_get16(msg) != g_rx_id)
        return;
    memcpy(g_rx, msg, len);
    g_rx_len = (u32)len;
}

/* xorshift64: enough to keep the id unpredictable to an off-path attacker.
 * A counter plus a fixed local port made a blind forgery practical. */
static u64 dns_rng_next(void)
{
    u64 x = g_rng;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}

static u16 dns_next_id(void)
{
    u16 id = (u16)(dns_rng_next() >> 32);

    if (id == 0u)
        id = 0x4352u;
    return id;
}

void dns_init(void)
{
    u32 i;

    memset(g_cache, 0, sizeof(g_cache));
    g_server = 0;
    g_server_port = (u16)NET_DNS_PORT;
    g_force_tcp = 0;
    g_name[0] = 0;
    g_transport = "udp";
    g_rx_len = 0;
    g_query_server = 0;
    g_rng ^= (u64)(timer_monotonic_ns() >> 3);
    if (g_rng == 0u)
        g_rng = 0x9E3779B97F4A7C15ull;
    g_rx_id = dns_next_id();
    /* Phase S1: two local ports, picked per query.  A fixed port gave an
     * off-path attacker a stable target; the second one is a spare slot in
     * the UDP handler table (DHCP keeps the other). */
    for (i = 0; i < NET_DNS_LOCAL_PORTS; i++)
        (void)net_udp_register((u16)(NET_DNS_LOCAL_PORT + i), dns_udp_handler);
}

void dns_cache_flush(void)
{
    memset(g_cache, 0, sizeof(g_cache));
}

const char *net_dns_transport(void)
{
    return g_transport;
}

static int dns_udp_query(const char *name, u32 server, u16 port, u32 timeout_ms,
                         struct net_dns_answer *answer, char *cname, usize cname_cap)
{
    const struct net_config *config = net_config_get();
    u8 mac[NET_MAC_LEN];
    u8 query[NET_DNS_MSG_MAX];
    usize qlen;
    u64 deadline;
    u32 arp_ms = timeout_ms > DNS_ARP_TIMEOUT_MS ? DNS_ARP_TIMEOUT_MS : timeout_ms;

    if (!config)
        return -1;
    if (net_arp_resolve(server, mac, arp_ms) != 0)
        return -1;
    /* Phase S1: remember where the answer must come from and which of the
     * two local ports carries it, then send the query from that port. */
    g_query_server = server;
    g_local_port = (u16)(NET_DNS_LOCAL_PORT + (u16)(dns_rng_next() & 1u));
    g_rx_id = dns_next_id();
    g_rx_len = 0;
    qlen = dns_build_query(name, g_rx_id, query, sizeof(query));
    if (!qlen)
        return -1;
    if (net_udp_send(mac, config->ip, server, g_local_port, port,
                     query, qlen) != 0)
        return -1;
    deadline = timer_monotonic_ns() + (u64)timeout_ms * 1000000ull;
    while (timer_monotonic_ns() < deadline) {
        int parsed;

        net_poll();
        if (!g_rx_len) {
            dns_idle();
            continue;
        }
        parsed = dns_parse_response(g_rx, (usize)g_rx_len, g_rx_id, answer,
                                    cname, cname_cap);
        g_rx_len = 0;
        if (parsed != 0)
            return parsed;
        return answer->count ? 0 : 1;
    }
    return -1;
}

static int dns_tcp_close(struct net_tcp_socket *s)
{
    (void)tcp_close(s, 1000u);
    tcp_socket_release(s);
    return -1;
}

static int dns_tcp_query(const char *name, u32 server, u16 port, u32 timeout_ms,
                         struct net_dns_answer *answer, char *cname, usize cname_cap)
{
    struct net_tcp_socket *sock;
    u8 framed[NET_DNS_MSG_MAX + 2u];
    u8 reply[NET_DNS_MSG_MAX];
    u8 lenbuf[2];
    usize qlen, got;
    u16 want;
    int n, parsed;

    sock = tcp_socket_new();
    if (!sock)
        return -1;
    if (tcp_connect(sock, server, port, timeout_ms) != 0)
        return dns_tcp_close(sock);
    g_rx_id = dns_next_id();
    qlen = dns_build_query(name, g_rx_id, framed + 2u, sizeof(framed) - 2u);
    if (!qlen)
        return dns_tcp_close(sock);
    net_put16(framed, (u16)qlen);
    if (tcp_send(sock, framed, qlen + 2u) < 0)
        return dns_tcp_close(sock);
    got = 0;
    while (got < 2u) {
        n = tcp_recv(sock, lenbuf + got, 2u - got, timeout_ms);
        if (n <= 0)
            return dns_tcp_close(sock);
        got += (usize)n;
    }
    want = net_get16(lenbuf);
    if (want < DNS_HEADER_BYTES || want > NET_DNS_MSG_MAX)
        return dns_tcp_close(sock);
    got = 0;
    while (got < (usize)want) {
        n = tcp_recv(sock, reply + got, (usize)want - got, timeout_ms);
        if (n <= 0)
            return dns_tcp_close(sock);
        got += (usize)n;
    }
    (void)tcp_close(sock, 1000u);
    tcp_socket_release(sock);
    parsed = dns_parse_response(reply, (usize)want, g_rx_id, answer, cname, cname_cap);
    if (parsed != 0)
        return parsed;
    return answer->count ? 0 : 1;
}

int net_dns_resolve(const char *name, struct net_dns_answer *answer, u32 timeout_ms)
{
    const struct net_config *config;
    struct net_dns_answer local;
    char cname[NET_DNS_NAME_MAX];
    char next[NET_DNS_NAME_MAX];
    u32 server, chain;
    int r;

    if (!answer || !name || !name[0])
        return -1;
    answer->count = 0;
    answer->ttl = 0;
    if (timeout_ms == 0u)
        timeout_ms = 3000u;
    if (dns_cache_lookup(name, answer) == 0) {
        g_transport = "cache";
        return 0;
    }
    config = net_config_get();
    server = g_server ? g_server : (config ? config->dns : 0u);
    if (!server)
        return -1;
    cname[0] = 0;
    local.count = 0;
    local.ttl = 0;
    for (chain = 0; chain <= NET_DNS_MAX_CNAME; chain++) {
        const char *query = chain == 0u ? name : cname;

        next[0] = 0;
        r = -1;
        if (!g_force_tcp) {
            r = dns_udp_query(query, server, g_server_port, timeout_ms, &local,
                              next, sizeof(next));
            if (r == 0 || r == 1)
                g_transport = "udp";
        }
        if (g_force_tcp || r == -1 || r == -2) {
            r = dns_tcp_query(query, server, g_server_port, timeout_ms, &local,
                              next, sizeof(next));
            if (r == 0 || r == 1)
                g_transport = "tcp";
        }
        if (r == 0) {
            *answer = local;
            dns_cache_store(name, answer);
            return 0;
        }
        if (r != 1 || !next[0] || chain == NET_DNS_MAX_CNAME)
            return -1;
        strncpy(cname, next, sizeof(cname) - 1u);
        cname[sizeof(cname) - 1u] = 0;
    }
    return -1;
}

static int dns_prefix(const char *tok, usize len, const char *prefix)
{
    usize n = 0;

    while (prefix[n])
        n++;
    if (len <= n)
        return 0;
    return memcmp(tok, prefix, n) == 0;
}

static int dns_parse_ipv4(const char *s, usize len, u32 *out)
{
    u32 parts[4];
    usize i = 0;
    u32 k;

    for (k = 0; k < 4u; k++) {
        u32 v = 0;
        u32 digits = 0;

        while (i < len && s[i] >= '0' && s[i] <= '9') {
            v = v * 10u + (u32)(s[i] - '0');
            if (v > 255u)
                return -1;
            digits++;
            i++;
        }
        if (digits == 0u)
            return -1;
        parts[k] = v;
        if (k < 3u) {
            if (i >= len || s[i] != '.')
                return -1;
            i++;
        }
    }
    if (i != len)
        return -1;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 0;
}

static void dns_cmdline_server(const char *val, usize len)
{
    usize i, hlen = len;
    u32 ip = 0, port = 0, k;
    int have_port = 0;

    for (i = 0; i < len; i++) {
        if (val[i] == ':') {
            hlen = i;
            have_port = 1;
            break;
        }
    }
    if (dns_parse_ipv4(val, hlen, &ip) != 0)
        return;
    if (have_port) {
        k = hlen + 1u;
        while (k < len && val[k] >= '0' && val[k] <= '9') {
            port = port * 10u + (u32)(val[k] - '0');
            k++;
        }
        if (k != len || port == 0u || port > 65535u)
            return;
    }
    g_server = ip;
    if (have_port)
        g_server_port = (u16)port;
}

static void dns_cmdline_token(const char *tok, usize len)
{
    static const char server_prefix[] = "net.dns=";
    static const char tcp_prefix[] = "net.dns-tcp=";
    static const char name_prefix[] = "net.name=";
    static const char test_prefix[] = "dns.test=";
    usize i;

    if (dns_prefix(tok, len, server_prefix)) {
        dns_cmdline_server(tok + sizeof(server_prefix) - 1u,
                           len - (sizeof(server_prefix) - 1u));
    } else if (dns_prefix(tok, len, tcp_prefix)) {
        usize off = sizeof(tcp_prefix) - 1u;
        u32 v = 0;

        for (i = off; i < len; i++) {
            if (tok[i] < '0' || tok[i] > '9')
                return;
            v = v * 10u + (u32)(tok[i] - '0');
        }
        g_force_tcp = v != 0u;
    } else if (dns_prefix(tok, len, name_prefix)) {
        usize off = sizeof(name_prefix) - 1u;
        usize n = len - off;

        if (n >= sizeof(g_name))
            n = sizeof(g_name) - 1u;
        memcpy(g_name, tok + off, n);
        g_name[n] = 0;
    } else if (dns_prefix(tok, len, test_prefix)) {
        /* dns.test=hostile runs the S1 hostile-chain and forged-reply
         * checks in the kernel (see dns_hostile_selftest). */
        usize off = sizeof(test_prefix) - 1u;

        if (len - off == 7u && memcmp(tok + off, "hostile", 7u) == 0)
            g_hostile_test = 1;
    }
}

/* Phase S1: mix the loader entropy into the id and port generator.  A
 * firmware without an RNG hands over an all-zero seed, in which case the
 * monotonic clock is the only source we have. */
static void dns_seed_entropy(const boot_info *bi)
{
    u64 mix = 0xD1B54A32D192ED03ull;
    usize i;

    if (!bi)
        return;
    for (i = 0; i < sizeof(bi->entropy_seed); i++)
        mix = (mix ^ (u64)bi->entropy_seed[i]) * 0x100000001B3ull;
    g_rng ^= mix;
    if (g_rng == 0u)
        g_rng = 0x9E3779B97F4A7C15ull;
}

void dns_cmdline(const boot_info *bi)
{
    const char *cmd;
    usize i, len;

    dns_seed_entropy(bi);
    if (!bi || !bi->command_line || bi->command_line_len == 0)
        return;
    cmd = bi->command_line;
    len = (usize)bi->command_line_len;
    i = 0;
    while (i < len) {
        usize start;

        while (i < len && (cmd[i] == ' ' || cmd[i] == '\t' || cmd[i] == '\n' || cmd[i] == '\r'))
            i++;
        start = i;
        while (i < len && cmd[i] != ' ' && cmd[i] != '\t' && cmd[i] != '\n' && cmd[i] != '\r')
            i++;
        if (i > start)
            dns_cmdline_token(cmd + start, i - start);
    }
}

int dns_selftest(void)
{
    struct net_dns_answer answer, cached;
    const struct net_config *config;
    char ipbuf[16];
    u32 server;

    if (net_device_count() == 0u) {
        kprintf("CRUGOS: dns selftest skipped (no network device)\n");
        return 1;
    }
    if (!g_name[0]) {
        kprintf("CRUGOS: dns selftest skipped (no net.name= token)\n");
        return 1;
    }
    config = net_config_get();
    server = g_server ? g_server : (config ? config->dns : 0u);
    if (!server) {
        kprintf("CRUGOS: net selftest failed (dns)\n");
        return 0;
    }
    dns_put_ip(ipbuf, sizeof(ipbuf), server);
    kprintf("CRUGOS: dns server %s:%u (%s)\n", ipbuf, (unsigned)g_server_port,
            g_force_tcp ? "tcp" : "udp");
    if (net_dns_resolve(g_name, &answer, 3000u) != 0 || answer.count == 0u) {
        kprintf("CRUGOS: net selftest failed (dns)\n");
        return 0;
    }
    dns_put_ip(ipbuf, sizeof(ipbuf), answer.addr[0]);
    kprintf("CRUGOS: dns resolved %s -> %s via %s (ttl %u)\n", g_name, ipbuf,
            net_dns_transport(), (unsigned)answer.ttl);
    if (net_dns_resolve(g_name, &cached, 3000u) != 0 || cached.count == 0u ||
        cached.addr[0] != answer.addr[0]) {
        kprintf("CRUGOS: net selftest failed (dns cache)\n");
        return 0;
    }
    dns_put_ip(ipbuf, sizeof(ipbuf), cached.addr[0]);
    kprintf("CRUGOS: dns cache hit %s -> %s\n", g_name, ipbuf);
    if (syscall_resolve_selftest(g_name, answer.addr[0]) != 0) {
        kprintf("CRUGOS: net selftest failed (dns socket)\n");
        return 0;
    }
    kprintf("CRUGOS: dns socket syscall ok\n");
    kprintf("CRUGOS: dns selftest ok\n");
    return 1;
}
/* Phase S1: the hostile selftest for the resolver.  It runs from net_init
 * when the command line carries dns.test=hostile, and proves two things the
 * audit asked for: a long backwards pointer chain is rejected by a flat
 * walk instead of eating the kernel stack, and a reply is only accepted
 * from the server and the port the query went to. */
static usize dns_hostile_chain(u8 *buf, usize links)
{
    usize base = DNS_HEADER_BYTES;
    usize p = base + 3u;
    usize i;

    memset(buf, 0, DNS_HOSTILE_BYTES);
    buf[base] = 1u;
    buf[base + 1u] = (u8)'a';
    buf[base + 2u] = 0u;
    for (i = 0; i < links; i++) {
        usize target = (i == 0u) ? base : (p - 2u);

        if (p + 2u > DNS_HOSTILE_BYTES)
            return 0;
        buf[p] = (u8)(0xC0u | (u8)((target >> 8) & 0x3Fu));
        buf[p + 1u] = (u8)(target & 0xFFu);
        p += 2u;
    }
    return p - 2u;
}

int dns_hostile_requested(void)
{
    return g_hostile_test;
}

int dns_hostile_selftest(void)
{
    static u8 buf[DNS_HOSTILE_BYTES];
    char name[64];
    u8 reply[DNS_HEADER_BYTES + 8u];
    usize pos;

    pos = dns_hostile_chain(buf, 12u);
    if (!pos || !dns_decode_name(buf, DNS_HOSTILE_BYTES, pos, name, sizeof(name)) ||
        strcmp(name, "a") != 0) {
        kprintf("CRUGOS: net selftest failed (dns hostile chain)\n");
        return 0;
    }
    pos = dns_hostile_chain(buf, 1200u);
    if (!pos) {
        kprintf("CRUGOS: net selftest failed (dns hostile chain)\n");
        return 0;
    }
    if (dns_decode_name(buf, DNS_HOSTILE_BYTES, pos, name, sizeof(name)) != 0) {
        kprintf("CRUGOS: net selftest failed (dns hostile chain)\n");
        return 0;
    }
    kprintf("CRUGOS: dns hostile chain ok (12 link(s) decoded, 1200 link(s) rejected)\n");

    memset(reply, 0, sizeof(reply));
    net_put16(reply, g_rx_id);
    g_query_server = 0x0A000202u;
    g_server_port = (u16)NET_DNS_PORT;
    g_local_port = (u16)NET_DNS_LOCAL_PORT;
    g_rx_len = 0;
    dns_udp_handler(0x0A0002FFu, (u16)NET_DNS_PORT, 0x0A00020Fu, g_local_port,
                    reply, sizeof(reply));
    if (g_rx_len != 0u) {
        kprintf("CRUGOS: net selftest failed (dns hostile source)\n");
        return 0;
    }
    dns_udp_handler(0x0A000202u, 54u, 0x0A00020Fu, g_local_port, reply, sizeof(reply));
    if (g_rx_len != 0u) {
        kprintf("CRUGOS: net selftest failed (dns hostile source)\n");
        return 0;
    }
    dns_udp_handler(0x0A000202u, (u16)NET_DNS_PORT, 0x0A00020Fu,
                    (u16)(g_local_port + 1u), reply, sizeof(reply));
    if (g_rx_len != 0u) {
        kprintf("CRUGOS: net selftest failed (dns hostile source)\n");
        return 0;
    }
    dns_udp_handler(0x0A000202u, (u16)NET_DNS_PORT, 0x0A00020Fu, g_local_port,
                    reply, sizeof(reply));
    if (g_rx_len == 0u) {
        kprintf("CRUGOS: net selftest failed (dns hostile source)\n");
        return 0;
    }
    g_rx_len = 0;
    g_query_server = 0;
    kprintf("CRUGOS: dns hostile source check ok (3 forged reply(ies) ignored)\n");
    kprintf("CRUGOS: dns hostile selftest ok\n");
    return 1;
}
