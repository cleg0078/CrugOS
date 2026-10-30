/* Legacy virtio-net PCI NIC: polled RX/TX queues, no interrupts. */
#include <crug/virtio.h>
#include <crug/net.h>
#include <crug/pci.h>
#include <crug/io.h>
#include <crug/mm.h>
#include <crug/heap.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include <crug/timer.h>

#define VIRTQ_MAX 256u
#define VIRTIO_NET_MAX 4u
#define VIRTIO_NET_RX_BUFFERS 32u
#define VIRTIO_NET_BUF_SIZE 2048u
#define VIRTIO_NET_HDR 10u
#define VIRTIO_NET_F_MAC (1u << 5)
#define VIRTQ_DESC_F_WRITE 2u

struct virtq_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} __attribute__((packed));

struct virtq_avail {
    u16 flags;
    u16 idx;
    u16 ring[VIRTQ_MAX];
};

struct virtq_used_elem {
    u32 id;
    u32 len;
};

struct virtq_used {
    u16 flags;
    u16 idx;
    struct virtq_used_elem ring[VIRTQ_MAX];
};

struct virtio_net {
    struct net_device nd;
    u16 io;
    u16 qsize_rx;
    u16 qsize_tx;
    u64 rx_pa;
    u64 tx_pa;
    struct virtq_desc *rx_desc;
    struct virtq_avail *rx_avail;
    struct virtq_used *rx_used;
    struct virtq_desc *tx_desc;
    struct virtq_avail *tx_avail;
    struct virtq_used *tx_used;
    u8 *rx_bufs;
    u64 rx_bufs_pa;
    u8 *tx_buf;
    u64 tx_buf_pa;
    u16 rx_last_used;
    u16 tx_last_used;
    u16 rx_posted;
    int ready;
};

static struct virtio_net g_nets[VIRTIO_NET_MAX];

static int vnet_queue(struct virtio_net *v, u16 qidx, u16 *out_qsize,
                      struct virtq_desc **out_desc, struct virtq_avail **out_avail,
                      struct virtq_used **out_used, u64 *out_pa)
{
    u16 qsize;
    u64 desc_bytes;
    u64 avail_bytes;
    u64 used_off;
    u64 used_end;
    u64 pages;
    u64 pa;

    outw(v->io + 0x0E, qidx);
    qsize = inw(v->io + 0x0C);
    if (qsize < 2 || (qsize & (u16)(qsize - 1)))
        return -1;
    if (qsize > VIRTQ_MAX)
        qsize = VIRTQ_MAX;
    desc_bytes = (u64)qsize * 16u;
    avail_bytes = 6u + 2u * (u64)qsize;
    used_off = (desc_bytes + avail_bytes + 4095ull) & ~4095ull;
    used_end = used_off + 6u + 8u * (u64)qsize;
    pages = (used_end + 4095ull) / 4096ull;
    pa = pmm_alloc_pages(pages);
    if (!pa)
        return -1;
    *out_desc = (struct virtq_desc *)pmm_phys_to_virt(pa);
    *out_avail = (struct virtq_avail *)((u8 *)*out_desc + desc_bytes);
    *out_used = (struct virtq_used *)((u8 *)*out_desc + used_off);
    *out_qsize = qsize;
    *out_pa = pa;
    outl(v->io + 0x08, (u32)(pa >> 12));
    return 0;
}

static void vnet_post_rx(struct virtio_net *v, u16 id)
{
    v->rx_avail->ring[v->rx_avail->idx % v->qsize_rx] = id;
    __sync_synchronize();
    v->rx_avail->idx++;
    __sync_synchronize();
    outw(v->io + 0x10, 0);
}

static void vnet_poll(struct net_device *nd)
{
    struct virtio_net *v = (struct virtio_net *)nd->priv;

    if (!v || !v->ready)
        return;
    __sync_synchronize();
    while (v->rx_used->idx != v->rx_last_used) {
        u16 slot = v->rx_last_used % v->qsize_rx;
        u32 id = v->rx_used->ring[slot].id;
        u32 used_len = v->rx_used->ring[slot].len;

        __sync_synchronize();
        v->rx_last_used++;
        /* Phase S1: the device writes used_len, so it is untrusted input.
           Clamp it to the buffer we actually posted before subtracting the
           virtio-net header; otherwise a lying device reads past the ring. */
        if (used_len > VIRTIO_NET_BUF_SIZE)
            used_len = VIRTIO_NET_BUF_SIZE;
        if (id < VIRTIO_NET_RX_BUFFERS && used_len > VIRTIO_NET_HDR) {
            net_receive(nd, v->rx_bufs + (u64)id * VIRTIO_NET_BUF_SIZE + VIRTIO_NET_HDR,
                        (usize)(used_len - VIRTIO_NET_HDR));
        }
        if (id < VIRTIO_NET_RX_BUFFERS)
            vnet_post_rx(v, (u16)id);
    }
}

static int vnet_send(struct net_device *nd, const void *frame, usize len)
{
    struct virtio_net *v = (struct virtio_net *)nd->priv;
    u64 deadline;

    if (!v || !v->ready || !frame)
        return -1;
    if (len > VIRTIO_NET_BUF_SIZE - VIRTIO_NET_HDR)
        return -1;
    memset(v->tx_buf, 0, VIRTIO_NET_HDR);
    memcpy(v->tx_buf + VIRTIO_NET_HDR, frame, len);
    v->tx_desc[0].addr = v->tx_buf_pa;
    v->tx_desc[0].len = (u32)(len + VIRTIO_NET_HDR);
    v->tx_desc[0].flags = 0;
    v->tx_desc[0].next = 0;
    v->tx_avail->ring[v->tx_avail->idx % v->qsize_tx] = 0;
    __sync_synchronize();
    v->tx_avail->idx++;
    __sync_synchronize();
    outw(v->io + 0x10, 1);
    deadline = timer_monotonic_ns() + 500000000ull;
    while (v->tx_used->idx == v->tx_last_used) {
        if (timer_monotonic_ns() > deadline)
            return -1;
    }
    __sync_synchronize();
    v->tx_last_used = v->tx_used->idx;
    return 0;
}

static int vnet_probe(u32 index, u16 io)
{
    struct virtio_net *v = &g_nets[index];
    u32 host_features;
    u32 i;
    u64 rx_pages;

    memset(v, 0, sizeof(*v));
    v->io = io;
    outb(v->io + 0x12, 0);
    outb(v->io + 0x12, 1);
    outb(v->io + 0x12, 2);
    host_features = inl(v->io + 0x00);
    if (!(host_features & VIRTIO_NET_F_MAC)) {
        kprintf("virtio : net feature negotiation failed (no MAC)\n");
        return -1;
    }
    outl(v->io + 0x04, VIRTIO_NET_F_MAC);
    outb(v->io + 0x12, 8);
    if (inb(v->io + 0x12) & 0x80) {
        kprintf("virtio : net feature negotiation failed\n");
        return -1;
    }
    for (i = 0; i < NET_MAC_LEN; i++)
        v->nd.mac[i] = inb(v->io + 0x14 + (u16)i);
    if (vnet_queue(v, 0, &v->qsize_rx, &v->rx_desc, &v->rx_avail,
                   &v->rx_used, &v->rx_pa) != 0)
        return -1;
    if (vnet_queue(v, 1, &v->qsize_tx, &v->tx_desc, &v->tx_avail,
                   &v->tx_used, &v->tx_pa) != 0)
        return -1;
    if (v->qsize_rx < VIRTIO_NET_RX_BUFFERS)
        return -1;
    rx_pages = ((u64)VIRTIO_NET_RX_BUFFERS * VIRTIO_NET_BUF_SIZE + 4095ull) / 4096ull;
    v->rx_bufs_pa = pmm_alloc_pages(rx_pages);
    if (!v->rx_bufs_pa)
        return -1;
    v->rx_bufs = (u8 *)pmm_phys_to_virt(v->rx_bufs_pa);
    v->tx_buf_pa = pmm_alloc_pages(1);
    if (!v->tx_buf_pa)
        return -1;
    v->tx_buf = (u8 *)pmm_phys_to_virt(v->tx_buf_pa);
    for (i = 0; i < VIRTIO_NET_RX_BUFFERS; i++) {
        v->rx_desc[i].addr = v->rx_bufs_pa + (u64)i * VIRTIO_NET_BUF_SIZE;
        v->rx_desc[i].len = VIRTIO_NET_BUF_SIZE;
        v->rx_desc[i].flags = VIRTQ_DESC_F_WRITE;
        v->rx_desc[i].next = 0;
        v->rx_avail->ring[i] = (u16)i;
    }
    v->rx_avail->idx = VIRTIO_NET_RX_BUFFERS;
    v->rx_posted = VIRTIO_NET_RX_BUFFERS;
    __sync_synchronize();
    outw(v->io + 0x10, 0);
    v->nd.name[0] = 'e';
    v->nd.name[1] = 't';
    v->nd.name[2] = 'h';
    v->nd.name[3] = (char)('0' + index);
    v->nd.name[4] = 0;
    v->nd.mtu = NET_MTU;
    v->nd.send = vnet_send;
    v->nd.poll = vnet_poll;
    v->nd.priv = v;
    if (net_register(&v->nd) != 0)
        return -1;
    v->ready = 1;
    outb(v->io + 0x12, 4);
    if (inb(v->io + 0x12) & 0x80) {
        v->ready = 0;
        kprintf("virtio : net device failed\n");
        return -1;
    }
    kprintf("virtio : net %s mac %02x:%02x:%02x:%02x:%02x:%02x\n", v->nd.name,
            (unsigned)v->nd.mac[0], (unsigned)v->nd.mac[1], (unsigned)v->nd.mac[2],
            (unsigned)v->nd.mac[3], (unsigned)v->nd.mac[4], (unsigned)v->nd.mac[5]);
    return 0;
}

int virtio_net_init(void)
{
    u32 index;
    u32 found = 0;

    for (index = 0; index < VIRTIO_NET_MAX; index++) {
        u8 bus = 0;
        u8 dev = 0;
        u8 fn = 0;
        u16 bar;

        if (!pci_find_nth(VIRTIO_PCI_VENDOR, VIRTIO_PCI_DEVICE_NET, index, &bus, &dev, &fn))
            break;
        bar = pci_bar(bus, dev, fn, 0);
        if (!(bar & PCI_BAR_IO))
            continue;
        pci_enable(bus, dev, fn, PCI_COMMAND_IO | PCI_COMMAND_MASTER);
        if (vnet_probe(found, (u16)(bar & ~0x3u)) != 0)
            kprintf("virtio : net probe failed\n");
        else
            found++;
    }
    return (int)found;
}
