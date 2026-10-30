#ifndef CRUG_NET_H
#define CRUG_NET_H

#include <crug/types.h>
#include <crug/bootinfo.h>

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

/* TCP (phase 4C): a minimal in-order client/server transport. */
#define NET_TCP_MAX 4u
#define NET_TCP_RX_BYTES 4096u
#define NET_TCP_MSS 1400u
#define NET_TCP_DEFAULT_HTTP_PORT 8080u
#define NET_TCP_RETRIES 5u
/* Phase S1: a half-open connection older than this is reaped, and a SYN that
   cannot be admitted is answered with RST|ACK instead of being dropped. */
#define NET_TCP_SYN_TIMEOUT_NS 10000000000ull

/* DNS (phase 4D): a caching stub resolver over UDP with a TCP fallback. */
#define NET_DNS_PORT 53u
#define NET_DNS_LOCAL_PORT 53000u
/* Phase S1: two local ports, chosen per query, so an off-path attacker has
 * no stable port to aim at. */
#define NET_DNS_LOCAL_PORTS 2u
#define NET_DNS_MSG_MAX 512u
#define NET_DNS_NAME_MAX 128u
#define NET_DNS_CACHE 8u
#define NET_DNS_MAX_ADDRS 4u
#define NET_DNS_MAX_CNAME 4u

enum net_tcp_state {
    NET_TCP_CLOSED = 0,
    NET_TCP_LISTEN,
    NET_TCP_SYN_SENT,
    NET_TCP_SYN_RECEIVED,
    NET_TCP_ESTABLISHED,
    NET_TCP_FIN_WAIT_1,
    NET_TCP_FIN_WAIT_2,
    NET_TCP_CLOSE_WAIT,
    NET_TCP_LAST_ACK,
    NET_TCP_TIME_WAIT
};

/* Sequence numbers compare modulo 2^32 (RFC 793). */
static inline int net_seq_lt(u32 a, u32 b)
{
    return (int)(a - b) < 0;
}

static inline int net_seq_le(u32 a, u32 b)
{
    return (int)(a - b) <= 0;
}

static inline int net_seq_gt(u32 a, u32 b)
{
    return net_seq_lt(b, a);
}

/* One connection control block, its receive window and its accept state. */
struct net_tcp_socket {
    u32 state;
    u32 local_ip;
    u32 remote_ip;
    u16 local_port;
    u16 remote_port;
    u16 snd_wnd;
    u32 snd_una;
    u32 snd_nxt;
    u32 rcv_nxt;
    u32 listen_port;
    u32 accepted;
    u32 used;
    u32 peer_closed;
    u64 state_ns;             /* when the current state was entered */
    u32 rx_head;
    u32 rx_count;
    u8 rx[NET_TCP_RX_BYTES];
};

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

void net_init(const boot_info *bi);
void net_cmdline(const boot_info *bi);
int net_register(struct net_device *nd);
u64 net_device_count(void);
struct net_device *net_first(void);
void net_poll(void);
void net_receive(struct net_device *nd, const void *frame, usize len);

int net_eth_send(struct net_device *nd, const u8 *dst_mac, u16 ethertype,
                 const void *payload, usize len);
int net_ipv4_send_raw(const u8 *dst_mac, u32 src_ip, u32 dst_ip, u8 proto,
                      const void *payload, usize len);
int net_ipv4_send(u32 dst_ip, u8 proto, const void *payload, usize len);
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
u16 net_tcp_checksum(u32 src, u32 dst, const u8 *seg, usize len);
int net_tcp_checksum_ok(u32 src, u32 dst, const u8 *seg, usize len);
u16 net_udp_checksum(u32 src, u32 dst, const u8 *dat, usize len);
int net_udp_checksum_ok(u32 src, u32 dst, const u8 *dat, usize len);

/* TCP transport, kernel/net/tcp.c. */
void tcp_init(void);
void tcp_cmdline(const boot_info *bi);
void tcp_receive_segment(u32 src_ip, u32 dst_ip, const u8 *seg, usize len);
struct net_tcp_socket *tcp_socket_new(void);
void tcp_socket_release(struct net_tcp_socket *s);
int tcp_reap_stale(u64 now_ns);
int tcp_synflood_requested(void);
int tcp_synflood_selftest(void);
int tcp_connect(struct net_tcp_socket *s, u32 ip, u16 port, u32 timeout_ms);
int tcp_listen(struct net_tcp_socket *s, u16 port);
struct net_tcp_socket *tcp_accept(struct net_tcp_socket *s, u32 timeout_ms);
int tcp_send(struct net_tcp_socket *s, const void *data, usize len);
int tcp_recv(struct net_tcp_socket *s, void *data, usize len, u32 timeout_ms);
int tcp_close(struct net_tcp_socket *s, u32 timeout_ms);
int tcp_selftest(void);

/* DNS resolver, kernel/net/dns.c. */
struct net_dns_answer {
    u32 addr[NET_DNS_MAX_ADDRS];
    u32 count;
    u32 ttl;
};

void dns_init(void);
void dns_cmdline(const boot_info *bi);
void dns_cache_flush(void);
int net_dns_resolve(const char *name, struct net_dns_answer *answer, u32 timeout_ms);
const char *net_dns_transport(void);
int dns_selftest(void);
int dns_hostile_requested(void);
int dns_hostile_selftest(void);
usize dns_build_query(const char *name, u16 id, u8 *out, usize cap);
int dns_parse_response(const u8 *msg, usize len, u16 id,
                       struct net_dns_answer *answer, char *cname, usize cname_cap);
usize dns_decode_name(const u8 *msg, usize len, usize pos, char *out, usize cap);
int net_arp_resolve(u32 ip, u8 mac[NET_MAC_LEN], u32 timeout_ms);

void dhcp_init(void);
int dhcp_acquire(u32 timeout_ms, struct net_config *config);

#endif /* CRUG_NET_H */
