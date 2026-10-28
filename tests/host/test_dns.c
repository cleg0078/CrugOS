/* Host tests for the phase 4D caching DNS stub resolver (kernel/net/dns.c). */
#include <crug/net.h>
#include <crug/types.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

/* ---------------------------------------------------------------------- */
/* Fakes: the resolver is exercised against a scripted network.           */
/* ---------------------------------------------------------------------- */

static u64 g_now;
static struct net_config g_config;
static void (*g_udp_handler)(u32, u16, u32, u16, const void *, usize);

/* UDP behaviour: 0 silent (timeout), 1 A answer, 2 truncated, 3 CNAME chain. */
static int g_udp_mode;
static int g_udp_calls;
static char g_udp_name[NET_DNS_NAME_MAX];
static u32 g_udp_addr = 0x0A000263u; /* 10.0.2.99 */
static u32 g_udp_ttl = 60u;
static char g_cname_target[NET_DNS_NAME_MAX] = "real.test";
static u32 g_cname_addr = 0x0A000264u; /* 10.0.2.100 */

static int g_tcp_connect_calls;
static u16 g_tcp_port;
static int g_tcp_send_calls;
static char g_tcp_name[NET_DNS_NAME_MAX];
static u8 g_tcp_reply[NET_DNS_MSG_MAX + 2u];
static usize g_tcp_reply_len;
static usize g_tcp_off;

static struct net_tcp_socket g_sock;

void kprintf(const char *fmt, ...)
{
    (void)fmt;
}

u64 timer_monotonic_ns(void)
{
    return g_now;
}

void net_poll(void)
{
    g_now += 1000000ull; /* 1 ms per poll, so timeout loops always end */
}

u64 net_device_count(void)
{
    return 1u;
}

const struct net_config *net_config_get(void)
{
    return &g_config;
}

int net_udp_register(u16 port,
                     void (*handler)(u32 src_ip, u16 src_port, u32 dst_ip,
                                     u16 dst_port, const void *data, usize len))
{
    (void)port;
    g_udp_handler = handler;
    return 0;
}

int net_arp_resolve(u32 ip, u8 mac[NET_MAC_LEN], u32 timeout_ms)
{
    (void)ip;
    (void)timeout_ms;
    mac[0] = 0x02u;
    mac[1] = 0x00u;
    mac[2] = 0x00u;
    mac[3] = 0x00u;
    mac[4] = 0x00u;
    mac[5] = 0x01u;
    return 0;
}

int syscall_resolve_selftest(const char *host, u32 addr)
{
    (void)host;
    (void)addr;
    return 0;
}

/* ---------------------------- reply builders --------------------------- */

static usize skip_query_name(const u8 *q, usize len)
{
    usize p = 12u;

    while (p < len) {
        u8 l = q[p];

        if (l == 0u) {
            p++;
            break;
        }
        if (l > 63u)
            return 0;
        p += 1u + (usize)l;
    }
    return p;
}

static usize put_name(u8 *out, const char *name)
{
    usize o = 0, i = 0;

    while (name[i]) {
        usize start = i;

        while (name[i] && name[i] != '.')
            i++;
        out[o++] = (u8)(i - start);
        memcpy(out + o, name + start, i - start);
        o += i - start;
        if (name[i] == '.')
            i++;
    }
    out[o++] = 0u;
    return o;
}

static usize reply_a(u8 *out, usize cap, const u8 *q, usize qlen, u32 addr,
                     u32 ttl, u16 flags)
{
    usize qend = skip_query_name(q, qlen);
    usize p;

    if (!qend || qend + 4u > qlen)
        return 0;
    qend += 4u;
    if (12u + (qend - 12u) + 16u > cap)
        return 0;
    memset(out, 0, 12u);
    memcpy(out, q, 2u);
    net_put16(out + 2u, flags);
    net_put16(out + 4u, 1u);
    net_put16(out + 6u, 1u);
    memcpy(out + 12u, q + 12u, qend - 12u);
    p = qend;
    out[p] = 0xC0u;
    out[p + 1u] = 0x0Cu;
    net_put16(out + p + 2u, 1u); /* A */
    net_put16(out + p + 4u, 1u); /* IN */
    net_put32(out + p + 6u, ttl);
    net_put16(out + p + 10u, 4u);
    net_put32(out + p + 12u, addr);
    return p + 16u;
}

static usize reply_cname(u8 *out, usize cap, const u8 *q, usize qlen,
                         const char *target, u32 ttl)
{
    usize qend = skip_query_name(q, qlen);
    u8 rdata[NET_DNS_NAME_MAX];
    usize rlen, p;

    if (!qend || qend + 4u > qlen)
        return 0;
    qend += 4u;
    rlen = put_name(rdata, target);
    if (12u + (qend - 12u) + 12u + rlen > cap)
        return 0;
    memset(out, 0, 12u);
    memcpy(out, q, 2u);
    net_put16(out + 2u, 0x8180u);
    net_put16(out + 4u, 1u);
    net_put16(out + 6u, 1u);
    memcpy(out + 12u, q + 12u, qend - 12u);
    p = qend;
    out[p] = 0xC0u;
    out[p + 1u] = 0x0Cu;
    net_put16(out + p + 2u, 5u); /* CNAME */
    net_put16(out + p + 4u, 1u);
    net_put32(out + p + 6u, ttl);
    net_put16(out + p + 10u, (u16)rlen);
    memcpy(out + p + 12u, rdata, rlen);
    return p + 12u + rlen;
}

/* ------------------------------ UDP fake ------------------------------- */

int net_udp_send(const u8 *dst_mac, u32 src_ip, u32 dst_ip, u16 src_port,
                 u16 dst_port, const void *payload, usize len)
{
    const u8 *q = (const u8 *)payload;
    u8 reply[NET_DNS_MSG_MAX + 2u];
    char name[NET_DNS_NAME_MAX];
    usize rlen = 0;

    (void)dst_mac;
    (void)src_ip;
    (void)dst_ip;
    (void)src_port;
    (void)dst_port;
    g_udp_calls++;
    if (g_udp_mode == 0 || !g_udp_handler || len < 12u || len > NET_DNS_MSG_MAX)
        return 0;
    if (!dns_decode_name(q, len, 12u, name, sizeof(name)))
        return 0;
    strncpy(g_udp_name, name, sizeof(g_udp_name) - 1u);
    g_udp_name[sizeof(g_udp_name) - 1u] = 0;
    if (g_udp_mode == 2) {
        memset(reply, 0, 12u);
        memcpy(reply, q, 2u);
        net_put16(reply + 2u, 0x8380u); /* response + truncated */
        net_put16(reply + 4u, 1u);
        net_put16(reply + 6u, 1u);
        rlen = 12u;
    } else if (g_udp_mode == 3 && strcmp(name, g_cname_target) != 0) {
        rlen = reply_cname(reply, sizeof(reply), q, len, g_cname_target,
                           g_udp_ttl);
    } else {
        u32 addr = (g_udp_mode == 3) ? g_cname_addr : g_udp_addr;

        rlen = reply_a(reply, sizeof(reply), q, len, addr, g_udp_ttl, 0x8180u);
    }
    if (!rlen)
        return 0;
    g_udp_handler(0x0A000202u, NET_DNS_PORT, g_config.ip, NET_DNS_LOCAL_PORT,
                  reply, rlen);
    return 0;
}

/* ------------------------------ TCP fake ------------------------------- */

struct net_tcp_socket *tcp_socket_new(void)
{
    return &g_sock;
}

void tcp_socket_release(struct net_tcp_socket *s)
{
    (void)s;
}

int tcp_connect(struct net_tcp_socket *s, u32 ip, u16 port, u32 timeout_ms)
{
    (void)s;
    (void)ip;
    (void)timeout_ms;
    g_tcp_connect_calls++;
    g_tcp_port = port;
    return 0;
}

int tcp_send(struct net_tcp_socket *s, const void *data, usize len)
{
    const u8 *q = (const u8 *)data;
    char name[NET_DNS_NAME_MAX] = {0};
    usize mlen, rlen;

    (void)s;
    g_tcp_send_calls++;
    if (len < 14u)
        return -1;
    mlen = net_get16(q);
    if (mlen + 2u != len || mlen > NET_DNS_MSG_MAX)
        return -1;
    if (!dns_decode_name(q + 2u, mlen, 12u, name, sizeof(name)))
        return -1;
    strncpy(g_tcp_name, name, sizeof(g_tcp_name) - 1u);
    g_tcp_name[sizeof(g_tcp_name) - 1u] = 0;
    rlen = reply_a(g_tcp_reply + 2u, sizeof(g_tcp_reply) - 2u, q + 2u, mlen,
                   g_udp_addr, g_udp_ttl, 0x8180u);
    if (!rlen)
        return -1;
    net_put16(g_tcp_reply, (u16)rlen);
    g_tcp_reply_len = rlen + 2u;
    g_tcp_off = 0;
    return (int)len;
}

int tcp_recv(struct net_tcp_socket *s, void *data, usize len, u32 timeout_ms)
{
    usize avail;

    (void)s;
    (void)timeout_ms;
    if (g_tcp_off >= g_tcp_reply_len)
        return -1;
    avail = g_tcp_reply_len - g_tcp_off;
    if (avail > len)
        avail = len;
    memcpy(data, g_tcp_reply + g_tcp_off, avail);
    g_tcp_off += avail;
    return (int)avail;
}

int tcp_close(struct net_tcp_socket *s, u32 timeout_ms)
{
    (void)s;
    (void)timeout_ms;
    return 0;
}

/* ------------------------------- helpers ------------------------------- */

static void reset_resolver(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.ip = 0x0A00020Fu; /* 10.0.2.15 */
    g_config.mask = 0xFFFFFF00u;
    g_config.gateway = 0x0A000202u;
    g_config.dns = 0x0A000202u; /* 10.0.2.2 */
    g_udp_mode = 1;
    g_udp_calls = 0;
    g_udp_name[0] = 0;
    g_tcp_connect_calls = 0;
    g_tcp_send_calls = 0;
    g_tcp_port = 0;
    g_tcp_name[0] = 0;
    g_tcp_off = 0;
    g_tcp_reply_len = 0;
    dns_init();
}

/* -------------------------------- tests -------------------------------- */

static void test_build_query(void)
{
    u8 buf[NET_DNS_MSG_MAX];
    char long_label[80];
    usize n;

    memset(buf, 0xAA, sizeof(buf));
    n = dns_build_query("crugos.test", 0x1234u, buf, sizeof(buf));
    CHECK(n == 29u);
    CHECK(net_get16(buf) == 0x1234u);
    CHECK(net_get16(buf + 2u) == 0x0100u);
    CHECK(net_get16(buf + 4u) == 1u);
    CHECK(net_get16(buf + 6u) == 0u);
    CHECK(buf[12] == 6u);
    CHECK(memcmp(buf + 13, "crugos", 6) == 0);
    CHECK(buf[19] == 4u);
    CHECK(memcmp(buf + 20, "test", 4) == 0);
    CHECK(buf[24] == 0u);
    CHECK(net_get16(buf + 25u) == 1u);
    CHECK(net_get16(buf + 27u) == 1u);

    CHECK(dns_build_query("", 1u, buf, sizeof(buf)) == 0u);
    CHECK(dns_build_query(NULL, 1u, buf, sizeof(buf)) == 0u);
    CHECK(dns_build_query("a..b", 1u, buf, sizeof(buf)) == 0u);
    CHECK(dns_build_query("crugos.test", 1u, buf, 20u) == 0u);
    CHECK(dns_build_query("crugos.test", 1u, NULL, 64u) == 0u);

    memset(long_label, 'a', 64u);
    long_label[64] = 0;
    CHECK(dns_build_query(long_label, 1u, buf, sizeof(buf)) == 0u);

    CHECK(dns_build_query("crugos.test.", 1u, buf, sizeof(buf)) == 29u);
}

static void test_decode_name(void)
{
    static const u8 plain[] = {3, 'a', 'b', 'c', 3, 'd', 'e', 'f', 0};
    static const u8 ptr[] = {3, 'w', 'w', 'w', 0, 0xC0, 0x00};
    static const u8 self[] = {0xC0, 0x00};
    static const u8 forward[] = {0xC0, 0x02, 0};
    static const u8 trunc[] = {3, 'a', 'b'};
    char out[NET_DNS_NAME_MAX];

    CHECK(dns_decode_name(plain, sizeof(plain), 0u, out, sizeof(out)) ==
          sizeof(plain));
    CHECK_STR(out, "abc.def");
    CHECK(dns_decode_name(ptr, sizeof(ptr), 5u, out, sizeof(out)) == 7u);
    CHECK_STR(out, "www");
    CHECK(dns_decode_name(self, sizeof(self), 0u, out, sizeof(out)) == 0u);
    CHECK(dns_decode_name(forward, sizeof(forward), 0u, out, sizeof(out)) == 0u);
    CHECK(dns_decode_name(trunc, sizeof(trunc), 0u, out, sizeof(out)) == 0u);
    CHECK(dns_decode_name(plain, sizeof(plain), 0u, out, 4u) == 0u);
    CHECK(dns_decode_name(plain, sizeof(plain), 12u, out, sizeof(out)) == 0u);
    CHECK(dns_decode_name(NULL, 4u, 0u, out, sizeof(out)) == 0u);
}

static void test_parse_response(void)
{
    u8 query[NET_DNS_MSG_MAX], reply[NET_DNS_MSG_MAX];
    struct net_dns_answer ans;
    char cname[NET_DNS_NAME_MAX];
    usize qlen, rlen;

    qlen = dns_build_query("crugos.test", 0x2222u, query, sizeof(query));
    CHECK(qlen == 29u);

    rlen = reply_a(reply, sizeof(reply), query, qlen, 0x0A000263u, 60u, 0x8180u);
    CHECK(rlen == 45u);
    memset(&ans, 0, sizeof(ans));
    cname[0] = 'x';
    CHECK(dns_parse_response(reply, rlen, 0x2222u, &ans, cname,
                             sizeof(cname)) == 0);
    CHECK(ans.count == 1u);
    CHECK(ans.addr[0] == 0x0A000263u);
    CHECK(ans.ttl == 60u);
    CHECK(cname[0] == 0);

    CHECK(dns_parse_response(reply, rlen, 0x2223u, &ans, cname,
                             sizeof(cname)) == -1);
    CHECK(dns_parse_response(reply, 4u, 0x2222u, &ans, cname,
                             sizeof(cname)) == -1);

    /* a lookup reply is not an answer */
    {
        u8 bad[64];
        usize blen = reply_a(bad, sizeof(bad), query, qlen, 0x0A000263u, 60u,
                             0x0180u);

        CHECK(blen != 0u);
        CHECK(dns_parse_response(bad, blen, 0x2222u, &ans, cname,
                                 sizeof(cname)) == -1);
    }

    /* NXDOMAIN */
    rlen = reply_a(reply, sizeof(reply), query, qlen, 0x0A000263u, 60u,
                   0x8183u);
    CHECK(dns_parse_response(reply, rlen, 0x2222u, &ans, cname,
                             sizeof(cname)) == -1);

    /* truncated: ask again over TCP */
    rlen = reply_a(reply, sizeof(reply), query, qlen, 0x0A000263u, 60u,
                   0x8380u);
    CHECK(dns_parse_response(reply, rlen, 0x2222u, &ans, cname,
                             sizeof(cname)) == -2);

    /* CNAME only: the resolver follows the chain */
    rlen = reply_cname(reply, sizeof(reply), query, qlen, "real.test", 60u);
    CHECK(rlen != 0u);
    CHECK(dns_parse_response(reply, rlen, 0x2222u, &ans, cname,
                             sizeof(cname)) == 0);
    CHECK(ans.count == 0u);
    CHECK_STR(cname, "real.test");

    /* no answer records at all */
    rlen = reply_a(reply, sizeof(reply), query, qlen, 0x0A000263u, 60u,
                   0x8180u);
    net_put16(reply + 6u, 0u);
    CHECK(dns_parse_response(reply, rlen, 0x2222u, &ans, NULL, 0u) == -1);
}

static void test_resolve_udp_cache(void)
{
    struct net_dns_answer ans;

    reset_resolver();
    memset(&ans, 0, sizeof(ans));
    CHECK(net_dns_resolve("crugos.test", &ans, 100u) == 0);
    CHECK(ans.count == 1u);
    CHECK(ans.addr[0] == g_udp_addr);
    CHECK_STR(net_dns_transport(), "udp");
    CHECK(g_udp_calls == 1);
    CHECK_STR(g_udp_name, "crugos.test");

    /* the cache key is case insensitive: no second query */
    memset(&ans, 0, sizeof(ans));
    CHECK(net_dns_resolve("CRUGOS.test", &ans, 100u) == 0);
    CHECK_STR(net_dns_transport(), "cache");
    CHECK(ans.addr[0] == g_udp_addr);
    CHECK(g_udp_calls == 1);

    dns_cache_flush();
    CHECK(net_dns_resolve("crugos.test", &ans, 100u) == 0);
    CHECK_STR(net_dns_transport(), "udp");
    CHECK(g_udp_calls == 2);
}

static void test_resolve_ttl(void)
{
    struct net_dns_answer ans;

    reset_resolver();
    g_udp_ttl = 60u;
    CHECK(net_dns_resolve("crugos.test", &ans, 100u) == 0);
    CHECK(g_udp_calls == 1);
    g_now += 61ull * 1000000000ull;
    CHECK(net_dns_resolve("crugos.test", &ans, 100u) == 0);
    CHECK(g_udp_calls == 2);
}

static void test_resolve_tcp_fallback(void)
{
    struct net_dns_answer ans;

    /* silent UDP: the query is retried over TCP */
    reset_resolver();
    g_udp_mode = 0;
    memset(&ans, 0, sizeof(ans));
    CHECK(net_dns_resolve("crugos.test", &ans, 20u) == 0);
    CHECK_STR(net_dns_transport(), "tcp");
    CHECK(ans.addr[0] == g_udp_addr);
    CHECK(g_udp_calls == 1);
    CHECK(g_tcp_connect_calls == 1);
    CHECK(g_tcp_send_calls == 1);
    CHECK(g_tcp_port == NET_DNS_PORT);
    CHECK_STR(g_tcp_name, "crugos.test");

    /* a truncated reply switches immediately */
    reset_resolver();
    g_udp_mode = 2;
    CHECK(net_dns_resolve("crugos.test", &ans, 100u) == 0);
    CHECK_STR(net_dns_transport(), "tcp");
    CHECK(g_udp_calls == 1);
    CHECK(g_tcp_connect_calls == 1);
}

static void test_resolve_cname(void)
{
    struct net_dns_answer ans;

    reset_resolver();
    g_udp_mode = 3;
    memset(&ans, 0, sizeof(ans));
    CHECK(net_dns_resolve("alias.test", &ans, 100u) == 0);
    CHECK(ans.count == 1u);
    CHECK(ans.addr[0] == g_cname_addr);
    CHECK(g_udp_calls == 2);
    CHECK_STR(net_dns_transport(), "udp");

    /* the answer is cached under the name the caller asked for */
    CHECK(net_dns_resolve("alias.test", &ans, 100u) == 0);
    CHECK_STR(net_dns_transport(), "cache");
    CHECK(ans.addr[0] == g_cname_addr);
    CHECK(g_udp_calls == 2);
}

static void test_resolve_errors(void)
{
    struct net_dns_answer ans;

    reset_resolver();
    CHECK(net_dns_resolve(NULL, &ans, 100u) == -1);
    CHECK(net_dns_resolve("", &ans, 100u) == -1);
    CHECK(net_dns_resolve("crugos.test", NULL, 100u) == -1);

    /* no server anywhere */
    reset_resolver();
    g_config.dns = 0u;
    CHECK(net_dns_resolve("crugos.test", &ans, 100u) == -1);
    CHECK(g_udp_calls == 0);
}

static void test_cmdline(void)
{
    static const char cmd[] =
        "net.dns=10.0.2.2:5300 net.dns-tcp=1 net.name=crugos.test";
    boot_info bi;
    struct net_dns_answer ans;

    reset_resolver();
    memset(&bi, 0, sizeof(bi));
    bi.command_line = cmd;
    bi.command_line_len = (u64)(sizeof(cmd) - 1u);
    dns_cmdline(&bi);

    CHECK(net_dns_resolve("crugos.test", &ans, 100u) == 0);
    CHECK_STR(net_dns_transport(), "tcp");
    CHECK(g_udp_calls == 0);
    CHECK(g_tcp_port == 5300u);
    CHECK(ans.addr[0] == g_udp_addr);

    /* the boot self-test resolves, hits the cache and the syscall path */
    dns_init();
    dns_cmdline(&bi);
    CHECK(dns_selftest() == 1);

    /* an out-of-range port keeps the default */
    dns_init();
    {
        static const char bad[] = "net.dns=10.0.2.2:99999 net.dns-tcp=1";
        boot_info bad_bi;

        memset(&bad_bi, 0, sizeof(bad_bi));
        bad_bi.command_line = bad;
        bad_bi.command_line_len = (u64)(sizeof(bad) - 1u);
        dns_cmdline(&bad_bi);
        CHECK(net_dns_resolve("crugos.test", &ans, 100u) == 0);
        CHECK_STR(net_dns_transport(), "tcp");
        CHECK(g_tcp_port == NET_DNS_PORT);
    }
}

void test_dns_functions(void)
{
    test_build_query();
    test_decode_name();
    test_parse_response();
    test_resolve_udp_cache();
    test_resolve_ttl();
    test_resolve_tcp_fallback();
    test_resolve_cname();
    test_resolve_errors();
    test_cmdline();
}
