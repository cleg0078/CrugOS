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
