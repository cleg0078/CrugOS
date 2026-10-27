#ifndef CRUG_NET_H
#define CRUG_NET_H

#include <crug/types.h>

#define NET_NAME_MAX 16u
#define NET_MAC_LEN 6u
#define NET_MTU 1500u
#define NET_FRAME_MAX 1600u
#define NET_MAX_DEVICES 4u
#define NET_ARP_ENTRIES 8u
#define NET_UDP_HANDLERS 4u

#define NET_ETHERTYPE_IPV4 0x0800u
#define NET_ETHERTYPE_ARP  0x0806u

#define NET_IP_PROTO_ICMP 1u
#define NET_IP_PROTO_TCP  6u
#define NET_IP_PROTO_UDP  17u

#define NET_DHCP_CLIENT_PORT 68u
#define NET_DHCP_SERVER_PORT 67u

/* A network interface. Drivers fill one in and call net_register(). */
struct net_device {
    char name[NET_NAME_MAX];
    u8 mac[NET_MAC_LEN];
    u16 mtu;
    u16 reserved;
    int (*send)(struct net_device *nd, const void *frame, usize len);
    void (*poll)(struct net_device *nd);
    void *priv;
    struct net_device *next;
};

/* IPv4 addresses are stored as 0xAABBCCDD for A.B.C.D. */
struct net_config {
    u32 ip;
    u32 mask;
    u32 gateway;
    u32 dns;
};

struct net_dhcp_lease {
    u8 type;
    u32 yiaddr;
    u32 server;
    u32 mask;
    u32 gateway;
    u32 dns;
};

void net_init(void);
int net_register(struct net_device *nd);
u64 net_device_count(void);
struct net_device *net_first(void);
void net_poll(void);
void net_receive(struct net_device *nd, const void *frame, usize len);

int net_eth_send(struct net_device *nd, const u8 *dst_mac, u16 ethertype,
                 const void *payload, usize len);
int net_ipv4_send_raw(const u8 *dst_mac, u32 src_ip, u32 dst_ip, u8 proto,
                      const void *payload, usize len);
int net_udp_send(const u8 *dst_mac, u32 src_ip, u32 dst_ip, u16 src_port,
                 u16 dst_port, const void *payload, usize len);
int net_udp_register(u16 port,
                     void (*handler)(u32 src_ip, u16 src_port, u32 dst_ip,
                                     u16 dst_port, const void *data, usize len));
const struct net_config *net_config_get(void);
void net_set_config(const struct net_config *config);
int net_selftest(void);

/* Big-endian wire helpers. */
static inline u16 net_get16(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | (u16)p[1]);
}

static inline u32 net_get32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static inline void net_put16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static inline void net_put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

u32 net_checksum_add(u32 partial, const void *data, usize len);
u16 net_checksum_final(u32 sum);
u16 net_checksum(const void *data, usize len);
int net_dhcp_parse(const u8 *packet, usize len, u32 xid,
                   struct net_dhcp_lease *out);

void dhcp_init(void);
int dhcp_acquire(u32 timeout_ms, struct net_config *config);

#endif /* CRUG_NET_H */
