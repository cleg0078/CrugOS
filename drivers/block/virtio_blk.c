#include <crug/virtio.h>
#include <crug/block.h>
#include <crug/pci.h>
#include <crug/io.h>
#include <crug/mm.h>
#include <crug/heap.h>
#include <crug/timer.h>
#include <crug/string.h>
#include <crug/kernel.h>

#define VIRTQ_MAX 256u
#define VIRTIO_BLK_MAX 8

#define VIRTQ_DESC_F_NEXT  1u
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
    u16 used_event;
} __attribute__((packed));

struct virtq_used_elem {
    u32 id;
    u32 len;
} __attribute__((packed));

struct virtq_used {
    u16 flags;
    u16 idx;
    struct virtq_used_elem ring[VIRTQ_MAX];
    u16 avail_event;
} __attribute__((packed));

struct virtio_blk_hdr {
    u32 type;
    u32 reserved;
    u64 sector;
} __attribute__((packed));

struct virtio_blk_req {
    struct virtio_blk_hdr hdr;
    u8 data[CRUG_SECTOR_SIZE];
    u8 status;
} __attribute__((packed));

struct virtio_blk {
    struct block_device bd;
    u16 io;
    u16 qsize;
    u64 queue_pa;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    struct virtio_blk_req *req;
    u16 last_used;
    u32 features;
    int has_flush;
    int ready;
};

static struct virtio_blk g_vblk[VIRTIO_BLK_MAX];

static void virtio_status(u16 io, u8 status)
{
    outb((u16)(io + 0x12), status);
}

static u8 virtio_get_status(u16 io)
{
    return inb((u16)(io + 0x12));
}

static int virtio_queue_init(struct virtio_blk *v)
{
    u16 qsize;
    u64 pa;
    u8 status;

    outw((u16)(v->io + 0x0E), 0);
    qsize = inw((u16)(v->io + 0x0C));
    if (qsize < 2 || qsize > VIRTQ_MAX || (qsize & (qsize - 1)) != 0) {
        kprintf("virtio : unsupported queue size %u\n", qsize);
        return -1;
    }
    pa = pmm_alloc_pages(3);
    if (!pa)
        return -1;
    memset(pmm_phys_to_virt(pa), 0, 3 * CRUG_PAGE_SIZE);
    v->qsize = qsize;
    v->queue_pa = pa;
    v->desc = (struct virtq_desc *)pmm_phys_to_virt(pa);
    v->avail = (struct virtq_avail *)((u8 *)v->desc +
                                      (usize)qsize * sizeof(struct virtq_desc));
    v->used = (struct virtq_used *)pmm_phys_to_virt(pa + 2 * CRUG_PAGE_SIZE);
    v->last_used = 0;
    outl((u16)(v->io + 0x08), (u32)(pa >> 12));
    status = virtio_get_status(v->io);
    if (status == 0xFFu)
        return -1;
    return 0;
}

static int virtio_submit(struct virtio_blk *v, int write, u64 lba)
{
    u16 head;
    u64 deadline;

    v->req->status = 0xFFu;
    v->req->hdr.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    v->req->hdr.reserved = 0;
    v->req->hdr.sector = lba;

    v->desc[0].addr = pmm_virt_to_phys(&v->req->hdr);
    v->desc[0].len = sizeof(v->req->hdr);
    v->desc[0].flags = VIRTQ_DESC_F_NEXT;
    v->desc[0].next = 1;
    v->desc[1].addr = pmm_virt_to_phys(v->req->data);
    v->desc[1].len = CRUG_SECTOR_SIZE;
    v->desc[1].flags = VIRTQ_DESC_F_NEXT | (write ? 0u : VIRTQ_DESC_F_WRITE);
    v->desc[1].next = 2;
    v->desc[2].addr = pmm_virt_to_phys(&v->req->status);
    v->desc[2].len = 1;
    v->desc[2].flags = VIRTQ_DESC_F_WRITE;
    v->desc[2].next = 0;

    head = (u16)(v->avail->idx % v->qsize);
    v->avail->ring[head] = 0;
    __sync_synchronize();
    v->avail->idx++;
    __sync_synchronize();
    outw((u16)(v->io + 0x10), 0);

    deadline = timer_monotonic_ns() + 500000000ull;
    while (v->used->idx == v->last_used) {
        if (timer_monotonic_ns() > deadline)
            return -1;
    }
    __sync_synchronize();
    v->last_used = v->used->idx;
    if (v->req->status != 0)
        return -1;
    return 0;
}

static int virtio_submit_flush(struct virtio_blk *v)
{
    u16 head;
    u64 deadline;

    v->req->status = 0xFFu;
    v->req->hdr.type = VIRTIO_BLK_T_FLUSH;
    v->req->hdr.reserved = 0;
    v->req->hdr.sector = 0;
    v->desc[0].addr = pmm_virt_to_phys(&v->req->hdr);
    v->desc[0].len = sizeof(v->req->hdr);
    v->desc[0].flags = VIRTQ_DESC_F_NEXT;
    v->desc[0].next = 1;
    v->desc[1].addr = pmm_virt_to_phys(&v->req->status);
    v->desc[1].len = 1;
    v->desc[1].flags = VIRTQ_DESC_F_WRITE;
    v->desc[1].next = 0;

    head = (u16)(v->avail->idx % v->qsize);
    v->avail->ring[head] = 0;
    __sync_synchronize();
    v->avail->idx++;
    __sync_synchronize();
    outw((u16)(v->io + 0x10), 0);

    deadline = timer_monotonic_ns() + 500000000ull;
    while (v->used->idx == v->last_used) {
        if (timer_monotonic_ns() > deadline)
            return -1;
    }
    __sync_synchronize();
    v->last_used = v->used->idx;
    return v->req->status == 0 ? 0 : -1;
}

static int virtio_blk_flush(struct block_device *bd)
{
    struct virtio_blk *v = (struct virtio_blk *)bd->priv;

    if (!v->ready || !v->has_flush)
        return 0;
    return virtio_submit_flush(v);
}

static int virtio_blk_read(struct block_device *bd, u64 lba, u32 count, void *buf)
{
    struct virtio_blk *v = (struct virtio_blk *)bd->priv;
    u8 *out = (u8 *)buf;
    u32 i;

    if (!v->ready)
        return -1;
    for (i = 0; i < count; i++) {
        if (virtio_submit(v, 0, lba + i) != 0)
            return -1;
        memcpy(out + (u64)i * CRUG_SECTOR_SIZE, v->req->data, CRUG_SECTOR_SIZE);
    }
    return 0;
}

static int virtio_blk_write(struct block_device *bd, u64 lba, u32 count,
                            const void *buf)
{
    struct virtio_blk *v = (struct virtio_blk *)bd->priv;
    const u8 *in = (const u8 *)buf;
    u32 i;

    if (!v->ready)
        return -1;
    for (i = 0; i < count; i++) {
        memcpy(v->req->data, in + (u64)i * CRUG_SECTOR_SIZE, CRUG_SECTOR_SIZE);
        if (virtio_submit(v, 1, lba + i) != 0)
            return -1;
    }
    return 0;
}

static int virtio_blk_setup(struct virtio_blk *v, u16 io, u32 index)
{
    u64 capacity;
    u8 status;

    memset(v, 0, sizeof(*v));
    v->io = io;
    virtio_status(io, 0);
    virtio_status(io, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_status(io, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    {
        u32 host_features = inl(io);
        u32 guest_features = 0;

        if (host_features & (1u << VIRTIO_BLK_F_FLUSH))
            guest_features |= (1u << VIRTIO_BLK_F_FLUSH);
        v->features = guest_features;
        v->has_flush = guest_features != 0;
        outl((u16)(io + 0x04), guest_features);
    }
    virtio_status(io, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                      VIRTIO_STATUS_FEATURES_OK);
    if (!(virtio_get_status(io) & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("virtio : device rejected feature negotiation\n");
        virtio_status(io, VIRTIO_STATUS_FAILED);
        return -1;
    }
    if (virtio_queue_init(v) != 0) {
        kprintf("virtio : queue setup failed\n");
        virtio_status(io, VIRTIO_STATUS_FAILED);
        return -1;
    }
    v->req = (struct virtio_blk_req *)kzalloc(sizeof(struct virtio_blk_req));
    if (!v->req) {
        virtio_status(io, VIRTIO_STATUS_FAILED);
        return -1;
    }
    capacity = (u64)inl((u16)(io + 0x14)) |
               ((u64)inl((u16)(io + 0x18)) << 32);
    if (capacity == 0) {
        kprintf("virtio : zero capacity\n");
        virtio_status(io, VIRTIO_STATUS_FAILED);
        return -1;
    }
    v->bd.sectors = capacity;
    v->bd.sector_size = CRUG_SECTOR_SIZE;
    v->bd.read = virtio_blk_read;
    v->bd.write = virtio_blk_write;
    v->bd.flush = virtio_blk_flush;
    v->bd.priv = v;
    v->bd.name[0] = 'v';
    v->bd.name[1] = 'd';
    v->bd.name[2] = (char)('a' + index);
    v->bd.name[3] = 0;
    v->ready = 1;
    virtio_status(io, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                      VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    status = virtio_get_status(io);
    if (status & VIRTIO_STATUS_FAILED) {
        kprintf("virtio : driver failed\n");
        v->ready = 0;
        return -1;
    }
    if (block_register(&v->bd) != 0) {
        v->ready = 0;
        return -1;
    }
    kprintf("virtio : blk %s, %llu sectors, io=0x%x\n", v->bd.name,
            (unsigned long long)capacity, io);
    return 0;
}

int virtio_blk_init(void)
{
    u32 i;
    u32 count = 0;

    for (i = 0; i < VIRTIO_BLK_MAX; i++) {
        u8 bus, dev, fn;
        u32 bar;
        u16 io;

        if (!pci_find_nth(VIRTIO_PCI_VENDOR, VIRTIO_PCI_DEVICE_BLOCK, (int)i,
                          &bus, &dev, &fn))
            break;
        bar = pci_bar(bus, dev, fn, 0);
        if (!(bar & PCI_BAR_IO)) {
            kprintf("virtio : device %u has no I/O BAR\n", i);
            continue;
        }
        io = (u16)(bar & ~0x3u);
        pci_enable(bus, dev, fn, PCI_COMMAND_IO | PCI_COMMAND_MASTER);
        if (virtio_blk_setup(&g_vblk[count], io, count) != 0) {
            kprintf("virtio : setup failed for device %u\n", i);
            continue;
        }
        count++;
    }
    return (int)count;
}
