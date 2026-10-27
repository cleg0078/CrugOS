/* DHCP client: DISCOVER/OFFER/REQUEST/ACK over UDP broadcast. */
#include <crug/net.h>
#include <crug/kernel.h>
#include <crug/string.h>
#include <crug/timer.h>
#include <crug/cpu.h>

#define DHCP_COOKIE 0x63825363u
#define DHCP_DISCOVER 1u
#define DHCP_OFFER 2u
#define DHCP_REQUEST 3u
#define DHCP_ACK 5u
#define DHCP_NAK 6u
#define DHCP_MSG_BYTES 300u

static const u8 g_broadcast[NET_MAC_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static u32 g_xid;
static int g_registered;
static int g_offer;
static int g_ack;
static int g_nak;
static struct net_dhcp_lease g_offer_lease;
static struct net_dhcp_lease g_ack_lease;

static void dhcp_handle(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port,
                        const void *data, usize len)
{
    struct net_dhcp_lease lease;

    (void)src_ip;
    (void)src_port;
    (void)dst_ip;
    (void)dst_port;
    if (!data || net_dhcp_parse((const u8 *)data, len, g_xid, &lease) != 1)
        return;
    if (lease.type == DHCP_OFFER) {
        g_offer_lease = lease;
        g_offer = 1;
    } else if (lease.type == DHCP_ACK) {
        g_ack_lease = lease;
        g_ack = 1;
    } else if (lease.type == DHCP_NAK) {
        g_nak = 1;
    }
}

void dhcp_init(void)
{
    if (!g_registered) {
        if (net_udp_register(NET_DHCP_CLIENT_PORT, dhcp_handle) == 0)
            g_registered = 1;
    }
}

static usize dhcp_build(u8 *buf, usize cap, u8 type, u32 xid, const u8 *mac,
                        u32 requested, u32 server)
{
    usize off = 240;

    if (!buf || cap < DHCP_MSG_BYTES)
        return 0;
    memset(buf, 0, DHCP_MSG_BYTES);
    buf[0] = 1;
    buf[1] = 1;
    buf[2] = NET_MAC_LEN;
    buf[3] = 0;
    net_put32(buf + 4, xid);
    net_put16(buf + 8, 0);
    net_put16(buf + 10, 0x8000u);
    memcpy(buf + 28, mac, NET_MAC_LEN);
    net_put32(buf + 236, DHCP_COOKIE);
    buf[off++] = 53;
    buf[off++] = 1;
    buf[off++] = type;
    buf[off++] = 55;
    buf[off++] = 3;
    buf[off++] = 1;
    buf[off++] = 3;
    buf[off++] = 6;
    if (requested) {
        buf[off++] = 50;
        buf[off++] = 4;
        net_put32(buf + off, requested);
        off += 4;
    }
    if (server) {
        buf[off++] = 54;
        buf[off++] = 4;
        net_put32(buf + off, server);
        off += 4;
    }
    buf[off++] = 255;
    return DHCP_MSG_BYTES;
}

int dhcp_acquire(u32 timeout_ms, struct net_config *config)
{
    struct net_device *nd = net_first();
    struct net_dhcp_lease offer;
    u8 buf[DHCP_MSG_BYTES];
    u32 attempt;

    if (!nd || !config)
        return -1;
    dhcp_init();
    if (!g_registered)
        return -1;
    for (attempt = 0; attempt < 3; attempt++) {
        usize len;
        u64 deadline;
        u32 xid;

        xid = 0x43520000u + (attempt << 8) + (u32)(timer_ticks() & 0xFFu);
        g_xid = xid;
        g_offer = 0;
        g_ack = 0;
        g_nak = 0;
        memset(&g_offer_lease, 0, sizeof(g_offer_lease));
        memset(&g_ack_lease, 0, sizeof(g_ack_lease));
        len = dhcp_build(buf, sizeof(buf), DHCP_DISCOVER, xid, nd->mac, 0, 0);
        if (net_udp_send(g_broadcast, 0, 0xFFFFFFFFu, NET_DHCP_CLIENT_PORT,
                         NET_DHCP_SERVER_PORT, buf, len) != 0)
            continue;
        deadline = timer_monotonic_ns() + (u64)timeout_ms * 1000000ull;
        while (timer_monotonic_ns() < deadline && !g_offer && !g_nak) {
            net_poll();
            cpu_wait_for_irq();
        }
        if (!g_offer)
            continue;
        offer = g_offer_lease;
        len = dhcp_build(buf, sizeof(buf), DHCP_REQUEST, xid, nd->mac,
                         offer.yiaddr, offer.server);
        if (net_udp_send(g_broadcast, 0, 0xFFFFFFFFu, NET_DHCP_CLIENT_PORT,
                         NET_DHCP_SERVER_PORT, buf, len) != 0)
            continue;
        deadline = timer_monotonic_ns() + (u64)timeout_ms * 1000000ull;
        while (timer_monotonic_ns() < deadline && !g_ack && !g_nak) {
            net_poll();
            cpu_wait_for_irq();
        }
        if (g_ack) {
            config->ip = g_ack_lease.yiaddr;
            config->mask = g_ack_lease.mask ? g_ack_lease.mask : 0xFFFFFF00u;
            config->gateway = g_ack_lease.gateway;
            config->dns = g_ack_lease.dns;
            return 0;
        }
    }
    return -1;
}
