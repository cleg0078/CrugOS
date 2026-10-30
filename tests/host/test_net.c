/* Host tests for the byte-order independent network helpers. */
#include <crug/net.h>
#include <crug/types.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

static void test_wire_helpers(void)
{
    u8 buf[8];

    net_put16(buf, 0x0806u);
    CHECK(buf[0] == 0x08u);
    CHECK(buf[1] == 0x06u);
    CHECK(net_get16(buf) == 0x0806u);

    net_put32(buf, 0xAABBCCDDu);
    CHECK(buf[0] == 0xAAu);
    CHECK(buf[1] == 0xBBu);
    CHECK(buf[2] == 0xCCu);
    CHECK(buf[3] == 0xDDu);
    CHECK(net_get32(buf) == 0xAABBCCDDu);

    /* 0.0.0.0 and 255.255.255.255 survive the round trip. */
    net_put32(buf, 0u);
    CHECK(net_get32(buf) == 0u);
    net_put32(buf, 0xFFFFFFFFu);
    CHECK(net_get32(buf) == 0xFFFFFFFFu);
}

static void test_checksum(void)
{
    static const u8 ipv4_header[20] = {
        0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00,
        0x40, 0x11, 0xB8, 0x61, 0xC0, 0xA8, 0x00, 0x01,
        0xC0, 0xA8, 0x00, 0xC7,
    };
    static const u8 ab[2] = { 'a', 'b' };
    static const u8 c[1] = { 'c' };
    static const u8 zeros[4] = { 0, 0, 0, 0 };
    u8 odd[3];
    u8 header[20];

    /* A header that carries its own checksum folds to zero... */
    CHECK(net_checksum(ipv4_header, sizeof(ipv4_header)) == 0u);

    /* ...and with the checksum field zeroed the computed value is 0xB861. */
    memcpy(header, ipv4_header, sizeof(header));
    header[10] = 0;
    header[11] = 0;
    CHECK(net_checksum(header, sizeof(header)) == 0xB861u);

    /* Odd lengths pad the trailing byte in the high half of a word. */
    odd[0] = 'a';
    odd[1] = 'b';
    odd[2] = 'c';
    CHECK(net_checksum(odd, 3) == 0x3B9Du);
    CHECK(net_checksum(zeros, sizeof(zeros)) == 0xFFFFu);
    CHECK(net_checksum(ab, 2) == 0x9E9Du);

    /* Chaining two halves matches one pass over the whole buffer. */
    CHECK(net_checksum_final(net_checksum_add(net_checksum_add(0, ab, 2), c, 1)) ==
          net_checksum(odd, 3));
    CHECK(net_checksum_final(net_checksum_add(0, ipv4_header, 10) +
                             net_checksum_add(0, ipv4_header + 10, 10)) ==
          net_checksum(ipv4_header, sizeof(ipv4_header)));

    /* Data plus its own checksum word folds to zero. */
    {
        u8 buf[4];
        u16 sum = net_checksum(ab, 2);

        memcpy(buf, ab, 2);
        net_put16(buf + 2, sum);
        CHECK(net_checksum(buf, 4) == 0u);
    }
}

static usize dhcp_option(u8 *p, u8 code, const u8 *value, u8 len)
{
    p[0] = code;
    p[1] = len;
    if (len > 0)
        memcpy(p + 2, value, len);
    return (usize)len + 2u;
}

static usize dhcp_build(u8 *packet, u32 xid, u8 type, u32 yiaddr, u32 server)
{
    static const u8 mask[4] = { 255, 255, 255, 0 };
    static const u8 router[4] = { 10, 0, 2, 2 };
    static const u8 dns[4] = { 10, 0, 2, 3 };
    u8 be_server[4];
    usize off = 240;

    memset(packet, 0, 300);
    packet[0] = 2; /* BOOTREPLY */
    packet[1] = 1; /* Ethernet */
    packet[2] = 6; /* hardware address length */
    net_put32(packet + 4, xid);
    net_put32(packet + 16, yiaddr);
    packet[28] = 0x52;
    packet[29] = 0x54;
    packet[30] = 0x00;
    packet[31] = 0x12;
    packet[32] = 0x34;
    packet[33] = 0x56;
    net_put32(packet + 236, 0x63825363u);

    off += dhcp_option(packet + off, 53, &type, 1);
    off += dhcp_option(packet + off, 1, mask, 4);
    off += dhcp_option(packet + off, 3, router, 4);
    off += dhcp_option(packet + off, 6, dns, 4);
    net_put32(be_server, server);
    off += dhcp_option(packet + off, 54, be_server, 4);
    packet[off++] = 255;
    return off;
}

static void test_dhcp_parse(void)
{
    u8 packet[300];
    struct net_dhcp_lease lease;
    const u32 xid = 0x43520001u;
    const u32 yiaddr = 0x0A00020Fu; /* 10.0.2.15 */
    const u32 server = 0x0A000202u; /* 10.0.2.2 */
    usize len;

    len = dhcp_build(packet, xid, 2 /* OFFER */, yiaddr, server);
    CHECK(net_dhcp_parse(packet, len, xid, &lease) == 1);
    CHECK(lease.type == 2u);
    CHECK(lease.yiaddr == yiaddr);
    CHECK(lease.server == server);
    CHECK(lease.mask == 0xFFFFFF00u);
    CHECK(lease.gateway == 0x0A000202u);
    CHECK(lease.dns == 0x0A000203u);

    /* ACK keeps the parsed address fields. */
    len = dhcp_build(packet, xid, 5 /* ACK */, yiaddr, server);
    CHECK(net_dhcp_parse(packet, len, xid, &lease) == 1);
    CHECK(lease.type == 5u);
    CHECK(lease.yiaddr == yiaddr);

    /* A reply for another transaction is ignored. */
    CHECK(net_dhcp_parse(packet, len, xid + 1u, &lease) == 0);

    /* No message type option means no lease. */
    len = dhcp_build(packet, xid, 0 /* none */, yiaddr, server);
    CHECK(net_dhcp_parse(packet, len, xid, &lease) == 0);

    /* Truncated and cookie-less packets are rejected. */
    len = dhcp_build(packet, xid, 2, yiaddr, server);
    CHECK(net_dhcp_parse(packet, 239, xid, &lease) == 0);
    packet[236] = 0x12;
    CHECK(net_dhcp_parse(packet, len, xid, &lease) == 0);

    /* An option that claims to extend past the packet is rejected. */
    net_put32(packet + 236, 0x63825363u);
    packet[240] = 53;
    packet[241] = 200;
    CHECK(net_dhcp_parse(packet, len, xid, &lease) == 0);

    CHECK(net_dhcp_parse(NULL, len, xid, &lease) == 0);
    CHECK(net_dhcp_parse(packet, len, xid, NULL) == 0);
}

static void test_udp_checksum(void)
{
    /* 10.0.2.15:53000 -> 10.0.2.2:53, one 5-byte DNS-shaped payload. */
    u8 dat[13];
    u16 sum;

    net_put16(dat + 0, 53000);
    net_put16(dat + 2, 53);
    net_put16(dat + 4, 13);
    net_put16(dat + 6, 0u);
    dat[8] = 0x12u;
    dat[9] = 0x34u;
    dat[10] = 0x01u;
    dat[11] = 0x00u;
    dat[12] = 0x01u;

    sum = net_udp_checksum(0x0A00020Fu, 0x0A000202u, dat, sizeof(dat));
    CHECK(sum != 0u);
    CHECK(net_udp_checksum_ok(0x0A00020Fu, 0x0A000202u, dat, sizeof(dat)) == 0);

    net_put16(dat + 6, sum);
    CHECK(net_udp_checksum_ok(0x0A00020Fu, 0x0A000202u, dat, sizeof(dat)) == 1);

    /* A flipped payload byte must fail verification. */
    dat[8] ^= 0xFFu;
    CHECK(net_udp_checksum_ok(0x0A00020Fu, 0x0A000202u, dat, sizeof(dat)) == 0);
    dat[8] ^= 0xFFu;

    /* The pseudo header binds the datagram to its endpoints, so a reply from
       another address cannot reuse the checksum. */
    CHECK(net_udp_checksum_ok(0x0A000203u, 0x0A000202u, dat, sizeof(dat)) == 0);

    /* A zero checksum is legal for IPv4 UDP and must verify as present-zero,
       which udp_receive treats as "sender supplied none". */
    net_put16(dat + 6, 0u);
    CHECK(net_udp_checksum_ok(0x0A00020Fu, 0x0A000202u, dat, sizeof(dat)) == 0);
}
static void test_tcp_checksum(void)
{
    /* 10.0.2.15:49152 -> 10.0.2.2:4444, SYN, 6-byte payload "crugos". */
    u8 seg[26];
    u16 sum;

    net_put16(seg + 0, 49152);
    net_put16(seg + 2, 4444);
    net_put32(seg + 4, 0x11223344u);
    net_put32(seg + 8, 0u);
    seg[12] = (u8)(5u << 4);
    seg[13] = 0x02u; /* SYN */
    net_put16(seg + 14, 4096);
    net_put16(seg + 16, 0u);
    net_put16(seg + 18, 0u);
    seg[20] = 'c';
    seg[21] = 'r';
    seg[22] = 'u';
    seg[23] = 'g';
    seg[24] = 'o';
    seg[25] = 's';

    sum = net_tcp_checksum(0x0A00020Fu, 0x0A000202u, seg, sizeof(seg));
    CHECK(sum == 0x29BCu);
    CHECK(net_tcp_checksum_ok(0x0A00020Fu, 0x0A000202u, seg, sizeof(seg)) == 0);

    net_put16(seg + 16, sum);
    CHECK(net_tcp_checksum_ok(0x0A00020Fu, 0x0A000202u, seg, sizeof(seg)) == 1);

    /* A flipped payload byte must fail verification. */
    seg[20] ^= 0xFFu;
    CHECK(net_tcp_checksum_ok(0x0A00020Fu, 0x0A000202u, seg, sizeof(seg)) == 0);
    seg[20] ^= 0xFFu;

    /* The pseudo header binds the segment to its endpoints. */
    CHECK(net_tcp_checksum_ok(0x0A000203u, 0x0A000202u, seg, sizeof(seg)) == 0);
}

void test_net_functions(void)
{
    test_wire_helpers();
    test_checksum();
    test_dhcp_parse();
    test_tcp_checksum();
    test_udp_checksum();
}
