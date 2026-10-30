/* Byte-order independent helpers shared by the network stack and host tests. */
#include <crug/net.h>
#include <crug/string.h>

u32 net_checksum_add(u32 partial, const void *data, usize len)
{
    const u8 *p = (const u8 *)data;
    u32 sum = partial;

    while (len > 1) {
        sum += net_get16(p);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (u32)p[0] << 8;
    return sum;
}

u16 net_checksum_final(u32 sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (u16)~sum;
}

u16 net_checksum(const void *data, usize len)
{
    return net_checksum_final(net_checksum_add(0, data, len));
}

/* Phase S1: TCP and UDP share the pseudo-header scheme, so both families go
   through one helper.  A checksum computed with the wrong protocol byte looks
   plausible and never matches, which is exactly how a spoofed datagram used to
   slip past a stack that did not verify UDP at all. */
static u32 net_transport_sum(u32 src, u32 dst, u8 proto, const u8 *data,
                             usize len)
{
    u8 pseudo[12];
    u32 sum;

    net_put32(pseudo, src);
    net_put32(pseudo + 4, dst);
    pseudo[8] = 0;
    pseudo[9] = proto;
    net_put16(pseudo + 10, (u16)len);
    sum = net_checksum_add(0, pseudo, sizeof(pseudo));
    return net_checksum_add(sum, data, len);
}

u16 net_tcp_checksum(u32 src, u32 dst, const u8 *seg, usize len)
{
    return net_checksum_final(net_transport_sum(src, dst, NET_IP_PROTO_TCP,
                                                   seg, len));
}

int net_tcp_checksum_ok(u32 src, u32 dst, const u8 *seg, usize len)
{
    return net_checksum_final(net_transport_sum(src, dst, NET_IP_PROTO_TCP,
                                                   seg, len)) == 0;
}

u16 net_udp_checksum(u32 src, u32 dst, const u8 *dat, usize len)
{
    return net_checksum_final(net_transport_sum(src, dst, NET_IP_PROTO_UDP,
                                                   dat, len));
}

int net_udp_checksum_ok(u32 src, u32 dst, const u8 *dat, usize len)
{
    return net_checksum_final(net_transport_sum(src, dst, NET_IP_PROTO_UDP,
                                                   dat, len)) == 0;
}

int net_dhcp_parse(const u8 *packet, usize len, u32 xid,
                   struct net_dhcp_lease *out)
{
    usize off;

    if (!packet || !out || len < 240)
        return 0;
    if (net_get32(packet + 4) != xid)
        return 0;
    if (net_get32(packet + 236) != 0x63825363u)
        return 0;

    memset(out, 0, sizeof(*out));
    out->yiaddr = net_get32(packet + 16);
    off = 240;
    while (off < len) {
        u8 option = packet[off++];
        u8 olen;

        if (option == 0)
            continue;
        if (option == 255)
            break;
        if (off >= len)
            return 0;
        olen = packet[off++];
        if ((usize)olen > len - off)
            return 0;
        switch (option) {
        case 53:
            if (olen >= 1)
                out->type = packet[off];
            break;
        case 1:
            if (olen >= 4)
                out->mask = net_get32(packet + off);
            break;
        case 3:
            if (olen >= 4)
                out->gateway = net_get32(packet + off);
            break;
        case 6:
            if (olen >= 4)
                out->dns = net_get32(packet + off);
            break;
        case 54:
            if (olen >= 4)
                out->server = net_get32(packet + off);
            break;
        default:
            break;
        }
        off += olen;
    }
    return out->type ? 1 : 0;
}
