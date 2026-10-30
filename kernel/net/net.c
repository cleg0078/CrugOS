/* CrugOS network stack: Ethernet, ARP, IPv4, ICMP echo and UDP dispatch. */
#include <crug/net.h>
#include <crug/virtio.h>
#include <crug/kernel.h>
#include <crug/string.h>
#include <crug/mm.h>
#include <crug/cpu.h>
#include <crug/timer.h>

static const u8 g_broadcast_mac[NET_MAC_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

struct arp_entry {
    u32 ip;
    u8 mac[NET_MAC_LEN];
    int valid;
};

struct udp_handler {
    u16 port;
    void (*handler)(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port,
                    const void *data, usize len);
};

static struct net_device *g_devices;
static u64 g_device_count;
static struct net_config g_config;
static struct arp_entry g_arp[NET_ARP_ENTRIES];
static struct udp_handler g_udp[NET_UDP_HANDLERS];
static u8 g_l3[NET_FRAME_MAX];
static u8 g_frame[NET_FRAME_MAX];
static u8 g_udp_buf[NET_FRAME_MAX];
static u32 g_ip_id;
static u16 g_icmp_id;
static u16 g_icmp_seq;
static u8 g_icmp_ttl;
static u32 g_icmp_src;
static int g_icmp_got;

void net_init(const boot_info *bi)
{
    g_devices = 0;
    g_device_count = 0;
    g_ip_id = 0x4352u;
    memset(&g_config, 0, sizeof(g_config));
    memset(g_arp, 0, sizeof(g_arp));
    memset(g_udp, 0, sizeof(g_udp));
    tcp_init();
    tcp_cmdline(bi);
    dns_init();
    dns_cmdline(bi);
    /* Phase S1: dns.test=hostile runs the resolver's hostile selftest.  Its
     * failure is fatal on purpose: a gate that accepts a stack-eating name
     * decoder or a forged reply would be worse than no gate at all. */
    if (dns_hostile_requested() && !dns_hostile_selftest())
        panic("dns hostile selftest failed");
    /* Phase S1: net.synflood=1 fills the connection table and proves the
       overflow RST and the half-open reaper actually recover it. */
    if (tcp_synflood_requested() && !tcp_synflood_selftest())
        panic("tcp syn flood selftest failed");
    dhcp_init();
    virtio_net_init();
}

int net_register(struct net_device *nd)
{
    struct net_device *it;

    if (!nd || !nd->send || nd->name[0] == 0)
        return -1;
    for (it = g_devices; it; it = it->next) {
        if (strcmp(it->name, nd->name) == 0)
            return -1;
    }
    nd->next = g_devices;
    g_devices = nd;
    g_device_count++;
    return 0;
}

u64 net_device_count(void)
{
    return g_device_count;
}

struct net_device *net_first(void)
{
    return g_devices;
}

void net_poll(void)
{
    struct net_device *it;

    for (it = g_devices; it; it = it->next) {
        if (it->poll)
            it->poll(it);
    }
}

int net_udp_register(u16 port,
                     void (*handler)(u32 src_ip, u16 src_port, u32 dst_ip,
                                     u16 dst_port, const void *data, usize len))
{
    u32 i;

    if (!handler)
        return -1;
    for (i = 0; i < NET_UDP_HANDLERS; i++) {
        if (g_udp[i].handler && g_udp[i].port == port) {
            g_udp[i].handler = handler;
            return 0;
        }
    }
    for (i = 0; i < NET_UDP_HANDLERS; i++) {
        if (!g_udp[i].handler) {
            g_udp[i].port = port;
            g_udp[i].handler = handler;
            return 0;
        }
    }
    return -1;
}

const struct net_config *net_config_get(void)
{
    return &g_config;
}

void net_set_config(const struct net_config *config)
{
    if (config)
        g_config = *config;
}

int net_eth_send(struct net_device *nd, const u8 *dst_mac, u16 ethertype,
                 const void *payload, usize len)
{
    usize frame_len;

    if (!nd || !nd->send || !dst_mac)
        return -1;
    if (len > NET_MTU || len + 14 > sizeof(g_frame))
        return -1;
    memcpy(g_frame, dst_mac, NET_MAC_LEN);
    memcpy(g_frame + 6, nd->mac, NET_MAC_LEN);
    net_put16(g_frame + 12, ethertype);
    if (len)
        memcpy(g_frame + 14, payload, len);
    frame_len = len + 14;
    if (frame_len < 60) {
        memset(g_frame + frame_len, 0, 60 - frame_len);
        frame_len = 60;
    }
    return nd->send(nd, g_frame, frame_len);
}

int net_ipv4_send_raw(const u8 *dst_mac, u32 src_ip, u32 dst_ip, u8 proto,
                      const void *payload, usize len)
{
    struct net_device *nd = net_first();
    usize total;

    if (!nd || !dst_mac)
        return -1;
    if (len > NET_MTU - 20)
        return -1;
    total = 20 + len;
    g_l3[0] = 0x45;
    g_l3[1] = 0;
    net_put16(g_l3 + 2, (u16)total);
    net_put16(g_l3 + 4, (u16)(++g_ip_id));
    net_put16(g_l3 + 6, 0);
    g_l3[8] = 64;
    g_l3[9] = proto;
    net_put16(g_l3 + 10, 0);
    net_put32(g_l3 + 12, src_ip);
    net_put32(g_l3 + 16, dst_ip);
    net_put16(g_l3 + 10, net_checksum(g_l3, 20));
    if (len)
        memcpy(g_l3 + 20, payload, len);
    return net_eth_send(nd, dst_mac, NET_ETHERTYPE_IPV4, g_l3, total);
}

int net_udp_send(const u8 *dst_mac, u32 src_ip, u32 dst_ip, u16 src_port,
                 u16 dst_port, const void *payload, usize len)
{
    u8 pseudo[12];
    usize total = 8 + len;
    u16 csum;

    if (total > NET_FRAME_MAX || !dst_mac)
        return -1;
    net_put16(g_udp_buf, src_port);
    net_put16(g_udp_buf + 2, dst_port);
    net_put16(g_udp_buf + 4, (u16)total);
    net_put16(g_udp_buf + 6, 0);
    if (len)
        memcpy(g_udp_buf + 8, payload, len);
    net_put32(pseudo, src_ip);
    net_put32(pseudo + 4, dst_ip);
    pseudo[8] = 0;
    pseudo[9] = NET_IP_PROTO_UDP;
    net_put16(pseudo + 10, (u16)total);
    csum = net_checksum_final(net_checksum_add(net_checksum_add(0, pseudo, sizeof(pseudo)),
                                               g_udp_buf, total));
    if (csum == 0)
        csum = 0xFFFFu;
    net_put16(g_udp_buf + 6, csum);
    return net_ipv4_send_raw(dst_mac, src_ip, dst_ip, NET_IP_PROTO_UDP,
                             g_udp_buf, total);
}

static int arp_lookup(u32 ip, u8 mac[NET_MAC_LEN])
{
    u32 i;

    for (i = 0; i < NET_ARP_ENTRIES; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            memcpy(mac, g_arp[i].mac, NET_MAC_LEN);
            return 1;
        }
    }
    return 0;
}

static void arp_cache_put(u32 ip, const u8 *mac)
{
    u32 i;
    u32 free_slot = NET_ARP_ENTRIES;

    for (i = 0; i < NET_ARP_ENTRIES; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            memcpy(g_arp[i].mac, mac, NET_MAC_LEN);
            return;
        }
        if (!g_arp[i].valid && free_slot == NET_ARP_ENTRIES)
            free_slot = i;
    }
    if (free_slot == NET_ARP_ENTRIES)
        free_slot = 0;
    g_arp[free_slot].ip = ip;
    memcpy(g_arp[free_slot].mac, mac, NET_MAC_LEN);
    g_arp[free_slot].valid = 1;
}

static void arp_send_request(u32 ip)
{
    struct net_device *nd = net_first();
    u8 request[28];
    u32 i;

    if (!nd)
        return;
    net_put16(request + 0, 1);
    net_put16(request + 2, NET_ETHERTYPE_IPV4);
    request[4] = NET_MAC_LEN;
    request[5] = 4;
    net_put16(request + 6, 1);
    memcpy(request + 8, nd->mac, NET_MAC_LEN);
    net_put32(request + 14, g_config.ip);
    for (i = 0; i < NET_MAC_LEN; i++)
        request[18 + i] = 0;
    net_put32(request + 24, ip);
    net_eth_send(nd, g_broadcast_mac, NET_ETHERTYPE_ARP, request, sizeof(request));
}

static void arp_receive(const u8 *p, usize len)
{
    u16 op;
    u32 sender_ip;
    u8 sender_mac[NET_MAC_LEN];

    if (len < 28)
        return;
    if (net_get16(p + 0) != 1 || net_get16(p + 2) != NET_ETHERTYPE_IPV4)
        return;
    if (p[4] != NET_MAC_LEN || p[5] != 4)
        return;
    op = net_get16(p + 6);
    memcpy(sender_mac, p + 8, NET_MAC_LEN);
    sender_ip = net_get32(p + 14);
    if (op == 2) {
        arp_cache_put(sender_ip, sender_mac);
    } else if (op == 1 && net_get32(p + 24) == g_config.ip) {
        struct net_device *nd = net_first();
        u8 reply[28];

        if (!nd)
            return;
        net_put16(reply + 0, 1);
        net_put16(reply + 2, NET_ETHERTYPE_IPV4);
        reply[4] = NET_MAC_LEN;
        reply[5] = 4;
        net_put16(reply + 6, 2);
        memcpy(reply + 8, nd->mac, NET_MAC_LEN);
        net_put32(reply + 14, g_config.ip);
        memcpy(reply + 18, sender_mac, NET_MAC_LEN);
        net_put32(reply + 24, sender_ip);
        net_eth_send(nd, sender_mac, NET_ETHERTYPE_ARP, reply, sizeof(reply));
    }
}

static int arp_resolve(u32 ip, u8 mac[NET_MAC_LEN], u32 timeout_ms)
{
    u32 attempt;
    u64 slice_ns = ((u64)timeout_ms * 1000000ull) / 3ull;

    if (arp_lookup(ip, mac))
        return 0;
    for (attempt = 0; attempt < 3; attempt++) {
        u64 deadline = timer_monotonic_ns() + slice_ns;

        arp_send_request(ip);
        while (timer_monotonic_ns() < deadline) {
            net_poll();
            if (arp_lookup(ip, mac))
                return 0;
            cpu_wait_for_irq();
        }
    }
    return -1;
}

static void udp_receive(const u8 *src, const u8 *dst, const u8 *udp, usize len)
{
    u16 src_port;
    u16 dst_port;
    u16 udp_len;
    u32 dst_ip;
    u32 i;

    if (len < 8)
        return;
    src_port = net_get16(udp);
    dst_port = net_get16(udp + 2);
    udp_len = net_get16(udp + 4);
    if (udp_len < 8 || (usize)udp_len > len)
        return;
    /* Phase S1: only datagrams addressed to us (or broadcast) are accepted,
       and the UDP checksum is verified when the sender supplied one.  A zero
       checksum is legal on IPv4 per RFC 768 and stays accepted. */
    dst_ip = net_get32(dst);
    if (dst_ip != net_config_get()->ip && dst_ip != 0xFFFFFFFFu)
        return;
    if (net_get16(udp + 6) != 0 &&
        !net_udp_checksum_ok(net_get32(src), dst_ip, udp, (usize)udp_len))
        return;
    for (i = 0; i < NET_UDP_HANDLERS; i++) {
        if (g_udp[i].handler && g_udp[i].port == dst_port) {
            g_udp[i].handler(net_get32(src), src_port, net_get32(dst), dst_port,
                             udp + 8, (usize)udp_len - 8);
            return;
        }
    }
}

static void icmp_receive(const u8 *ip, usize ihl, usize len)
{
    const u8 *icmp = ip + ihl;

    if (len < 8)
        return;
    if (icmp[0] != 0 || icmp[1] != 0)
        return;
    if (net_checksum(icmp, len) != 0)
        return;
    if (net_get16(icmp + 4) != g_icmp_id || net_get16(icmp + 6) != g_icmp_seq)
        return;
    g_icmp_src = net_get32(ip + 12);
    g_icmp_ttl = ip[8];
    g_icmp_got = 1;
}

static void ipv4_receive(const u8 *p, usize len)
{
    usize ihl;
    usize total;
    u8 proto;

    if (len < 20 || (p[0] >> 4) != 4)
        return;
    ihl = (usize)(p[0] & 0x0Fu) * 4;
    if (ihl < 20 || ihl > len)
        return;
    total = net_get16(p + 2);
    if (total < ihl || total > len)
        return;
    if (net_checksum(p, ihl) != 0)
        return;
    proto = p[9];
    if (proto == NET_IP_PROTO_ICMP)
        icmp_receive(p, ihl, total - ihl);
    else if (proto == NET_IP_PROTO_UDP)
        udp_receive(p + 12, p + 16, p + ihl, total - ihl);
    else if (proto == NET_IP_PROTO_TCP)
        tcp_receive_segment(net_get32(p + 12), net_get32(p + 16), p + ihl,
                            total - ihl);
}

void net_receive(struct net_device *nd, const void *frame, usize len)
{
    const u8 *f = (const u8 *)frame;
    u16 ethertype;

    if (!nd || !frame || len < 14)
        return;
    if (memcmp(f, nd->mac, NET_MAC_LEN) != 0 &&
        memcmp(f, g_broadcast_mac, NET_MAC_LEN) != 0)
        return;
    ethertype = net_get16(f + 12);
    if (ethertype == NET_ETHERTYPE_ARP)
        arp_receive(f + 14, len - 14);
    else if (ethertype == NET_ETHERTYPE_IPV4)
        ipv4_receive(f + 14, len - 14);
}

static int icmp_ping(u32 dst, u16 seq, u32 timeout_ms)
{
    struct net_device *nd = net_first();
    u8 mac[NET_MAC_LEN];
    u8 packet[8 + 16];
    u64 deadline;

    if (!nd)
        return -1;
    if (arp_resolve(dst, mac, timeout_ms) != 0)
        return -1;
    g_icmp_id = (u16)(0x4352u ^ (u16)timer_ticks());
    g_icmp_seq = seq;
    g_icmp_got = 0;
    packet[0] = 8;
    packet[1] = 0;
    net_put16(packet + 2, 0);
    net_put16(packet + 4, g_icmp_id);
    net_put16(packet + 6, seq);
    memcpy(packet + 8, "crugos-net-ping", 16);
    net_put16(packet + 2, net_checksum(packet, sizeof(packet)));
    if (net_ipv4_send_raw(mac, g_config.ip, dst, NET_IP_PROTO_ICMP, packet,
                          sizeof(packet)) != 0)
        return -1;
    deadline = timer_monotonic_ns() + (u64)timeout_ms * 1000000ull;
    while (timer_monotonic_ns() < deadline) {
        net_poll();
        if (g_icmp_got)
            return 0;
        cpu_wait_for_irq();
    }
    return -1;
}

int net_ipv4_send(u32 dst_ip, u8 proto, const void *payload, usize len)
{
    u8 mac[NET_MAC_LEN];

    if (arp_resolve(dst_ip, mac, 3000u) != 0)
        return -1;
    return net_ipv4_send_raw(mac, g_config.ip, dst_ip, proto, payload, len);
}

int net_selftest(void)
{
    struct net_device *nd = net_first();
    struct net_config config;
    u8 mac[NET_MAC_LEN];

    if (!nd) {
        kprintf("CRUGOS: net selftest skipped (no network device)\n");
        return 1;
    }
    if (dhcp_acquire(1000u, &config) != 0) {
        kprintf("CRUGOS: net selftest failed (dhcp)\n");
        return 0;
    }
    net_set_config(&config);
    kprintf("CRUGOS: dhcp lease %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u\n",
            (unsigned)(config.ip >> 24), (unsigned)((config.ip >> 16) & 0xFFu),
            (unsigned)((config.ip >> 8) & 0xFFu), (unsigned)(config.ip & 0xFFu),
            (unsigned)(config.mask >> 24), (unsigned)((config.mask >> 16) & 0xFFu),
            (unsigned)((config.mask >> 8) & 0xFFu), (unsigned)(config.mask & 0xFFu),
            (unsigned)(config.gateway >> 24), (unsigned)((config.gateway >> 16) & 0xFFu),
            (unsigned)((config.gateway >> 8) & 0xFFu), (unsigned)(config.gateway & 0xFFu),
            (unsigned)(config.dns >> 24), (unsigned)((config.dns >> 16) & 0xFFu),
            (unsigned)((config.dns >> 8) & 0xFFu), (unsigned)(config.dns & 0xFFu));
    if (arp_resolve(config.gateway, mac, 3000u) != 0) {
        kprintf("CRUGOS: net selftest failed (arp)\n");
        return 0;
    }
    kprintf("CRUGOS: arp resolved %u.%u.%u.%u %02x:%02x:%02x:%02x:%02x:%02x\n",
            (unsigned)(config.gateway >> 24), (unsigned)((config.gateway >> 16) & 0xFFu),
            (unsigned)((config.gateway >> 8) & 0xFFu), (unsigned)(config.gateway & 0xFFu),
            (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
            (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
    if (icmp_ping(config.gateway, 1, 3000u) != 0) {
        kprintf("CRUGOS: net selftest failed (icmp)\n");
        return 0;
    }
    kprintf("CRUGOS: icmp echo reply from %u.%u.%u.%u seq=1 ttl=%u\n",
            (unsigned)(g_icmp_src >> 24), (unsigned)((g_icmp_src >> 16) & 0xFFu),
            (unsigned)((g_icmp_src >> 8) & 0xFFu), (unsigned)(g_icmp_src & 0xFFu),
            (unsigned)g_icmp_ttl);
    if (!tcp_selftest()) {
        kprintf("CRUGOS: net selftest failed (tcp)\n");
        return 0;
    }
    if (!dns_selftest()) {
        kprintf("CRUGOS: net selftest failed (dns)\n");
        return 0;
    }
    kprintf("CRUGOS: net selftest ok\n");
    return 1;
}

int net_arp_resolve(u32 ip, u8 mac[NET_MAC_LEN], u32 timeout_ms)
{
    return arp_resolve(ip, mac, timeout_ms);
}
