/* NVMe over PCIe: polled admin and I/O queues, one namespace per controller. */
#include <crug/nvme.h>
#include <crug/types.h>
#include <crug/compiler.h>
#include <crug/pci.h>
#include <crug/block.h>
#include <crug/mm.h>
#include <crug/vm.h>
#include <crug/timer.h>
#include <crug/string.h>
#include <crug/kernel.h>

#define NVME_CAP        0x0000u
#define NVME_VS         0x0008u
#define NVME_INTMS      0x000Cu
#define NVME_INTMC      0x0010u
#define NVME_CC         0x0014u
#define NVME_CSTS       0x001Cu
#define NVME_AQA        0x0024u
#define NVME_ASQ        0x0028u
#define NVME_ACQ        0x0030u
#define NVME_DOORBELL   0x1000u

#define NVME_CC_EN      0x00000001u
#define NVME_CC_CSS_NVM 0x00000000u
#define NVME_CC_MPS_4K  0x00000000u
#define NVME_CC_IOSQES  0x00060000u
#define NVME_CC_IOCQES  0x00400000u
#define NVME_CSTS_RDY   0x00000001u
#define NVME_CSTS_CFS   0x00000002u

#define NVME_ADMIN_IDENTIFY 0x06u
#define NVME_ADMIN_CREATE_CQ 0x05u
#define NVME_ADMIN_CREATE_SQ 0x01u
#define NVME_IO_FLUSH   0x00u
#define NVME_IO_WRITE   0x01u
#define NVME_IO_READ    0x02u

#define NVME_QUEUE_ENTRIES 64u
#define NVME_MAX_PAGES   512u
#define NVME_TIMEOUT_MS  1000u

struct nvme_cmd {
    u32 d0;
    u32 d1;
    u32 d2;
    u32 d3;
    u32 d4;
    u32 d5;
    u64 prp1;
    u64 prp2;
    u32 cdw10;
    u32 cdw11;
    u32 cdw12;
    u32 cdw13;
    u32 cdw14;
    u32 cdw15;
} __attribute__((packed));

struct nvme_cpl {
    u32 result;
    u32 rsvd;
    u16 sq_head;
    u16 sq_id;
    u16 cid;
    u16 status;
} __attribute__((packed));

struct nvme_queue {
    struct nvme_cmd *sq;
    struct nvme_cpl *cq;
    u64 sq_pa;
    u64 cq_pa;
    u16 qid;
    u16 size;
    u16 sq_tail;
    u16 cq_head;
    u8 phase;
};

struct nvme_ctrl {
    int ready;
    volatile u8 *regs;
    u64 bar_pa;
    u64 bar_size;
    u32 stride;
    u16 qsize;
    u16 cid;
    u32 nsid;
    struct nvme_queue admin;
    struct nvme_queue io;
    struct block_device bd;
};

static struct nvme_ctrl g_ctrl[NVME_MAX_CONTROLLERS];
static u32 g_count;
static u8 g_identify[4096];

static u32 nvme_read32(struct nvme_ctrl *c, u32 off)
{
    return *(volatile u32 *)(c->regs + off);
}

static void nvme_write32(struct nvme_ctrl *c, u32 off, u32 value)
{
    *(volatile u32 *)(c->regs + off) = value;
}

static u64 nvme_read64(struct nvme_ctrl *c, u32 off)
{
    u64 lo = nvme_read32(c, off);
    u64 hi = nvme_read32(c, off + 4);
    return lo | (hi << 32);
}

static void nvme_write64(struct nvme_ctrl *c, u32 off, u64 value)
{
    nvme_write32(c, off, (u32)value);
    nvme_write32(c, off + 4, (u32)(value >> 32));
}

static void nvme_doorbell(struct nvme_ctrl *c, u16 qid, int cq, u16 value)
{
    u32 off = NVME_DOORBELL + (2u * qid + (cq ? 1u : 0u)) * c->stride;

    nvme_write32(c, off, value);
}

static int nvme_wait_ready(struct nvme_ctrl *c, int ready)
{
    u64 deadline = timer_monotonic_ns() + (u64)NVME_TIMEOUT_MS * 1000000ull;

    for (;;) {
        u32 csts = nvme_read32(c, NVME_CSTS);

        if ((csts & NVME_CSTS_CFS) && ready)
            return -1;
        if (!!(csts & NVME_CSTS_RDY) == !!ready)
            return 0;
        if ((i64)(timer_monotonic_ns() - deadline) >= 0)
            return -1;
    }
}

static int nvme_wait_cq(struct nvme_ctrl *c, struct nvme_queue *q,
                        struct nvme_cpl *out)
{
    u64 deadline = timer_monotonic_ns() + (u64)NVME_TIMEOUT_MS * 1000000ull;

    for (;;) {
        volatile struct nvme_cpl *entry = &q->cq[q->cq_head];
        u16 status = entry->status;

        __sync_synchronize();
        if ((status & 1u) == q->phase) {
            *out = *entry;
            q->cq_head++;
            if (q->cq_head == q->size) {
                q->cq_head = 0;
                q->phase ^= 1u;
            }
            nvme_doorbell(c, q->qid, 1, q->cq_head);
            return 0;
        }
        if ((i64)(timer_monotonic_ns() - deadline) >= 0)
            return -1;
    }
}

static int nvme_submit(struct nvme_ctrl *c, struct nvme_queue *q,
                       const struct nvme_cmd *cmd, struct nvme_cpl *cpl)
{
    u16 tail = q->sq_tail;

    q->sq[tail] = *cmd;
    q->sq_tail = (u16)((tail + 1u) % q->size);
    __sync_synchronize();
    nvme_doorbell(c, q->qid, 0, q->sq_tail);
    return nvme_wait_cq(c, q, cpl);
}

static int nvme_admin_cmd(struct nvme_ctrl *c, u8 opcode, u32 nsid,
                         u32 cdw10, u32 cdw11, u64 prp1)
{
    struct nvme_cmd cmd;
    struct nvme_cpl cpl;

    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.d0 = (u32)opcode | ((u32)c->cid++ << 16);
    cmd.d1 = nsid;
    cmd.prp1 = prp1;
    cmd.cdw10 = cdw10;
    cmd.cdw11 = cdw11;
    if (nvme_submit(c, &c->admin, &cmd, &cpl) != 0)
        return -1;
    return (int)(cpl.status >> 1);
}

static int nvme_io_cmd(struct nvme_ctrl *c, u8 opcode, u64 lba, u32 nlb,
                       u64 prp1, u64 prp2)
{
    struct nvme_cmd cmd;
    struct nvme_cpl cpl;

    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.d0 = (u32)opcode | ((u32)c->cid++ << 16);
    cmd.d1 = c->nsid;
    cmd.prp1 = prp1;
    cmd.prp2 = prp2;
    cmd.cdw10 = (u32)lba;
    cmd.cdw11 = (u32)(lba >> 32);
    cmd.cdw12 = nlb;
    if (nvme_submit(c, &c->io, &cmd, &cpl) != 0)
        return -1;
    return (int)(cpl.status >> 1);
}

static int nvme_alloc_queue(struct nvme_ctrl *c, struct nvme_queue *q,
                            u16 qid, u16 size)
{
    q->sq_pa = pmm_alloc_page();
    q->cq_pa = pmm_alloc_page();
    if (!q->sq_pa || !q->cq_pa)
        return -1;
    q->sq = (struct nvme_cmd *)pmm_phys_to_virt(q->sq_pa);
    q->cq = (struct nvme_cpl *)pmm_phys_to_virt(q->cq_pa);
    q->qid = qid;
    q->size = size;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->phase = 1;
    (void)c;
    return 0;
}

static void nvme_free_queue(struct nvme_queue *q)
{
    if (q->sq_pa)
        pmm_free_page(q->sq_pa);
    if (q->cq_pa)
        pmm_free_page(q->cq_pa);
    memset(q, 0, sizeof(*q));
}

static int nvme_disable(struct nvme_ctrl *c)
{
    u32 cc = nvme_read32(c, NVME_CC);

    if (cc & NVME_CC_EN) {
        nvme_write32(c, NVME_CC, cc & ~NVME_CC_EN);
        if (nvme_wait_ready(c, 0) != 0)
            return -1;
    }
    return 0;
}

static int nvme_bar_setup(struct nvme_ctrl *c, u8 bus, u8 dev, u8 fn)
{
    u32 bar0 = pci_read32(bus, dev, fn, 0x10);
    u16 cmd = pci_read16(bus, dev, fn, 0x04);
    int is64;
    u32 lo, hi, lo_mask, hi_mask;
    u64 mask, size, pa;
    u64 off;

    if (bar0 & PCI_BAR_IO)
        return -1;
    is64 = ((bar0 >> 1) & 3u) == 2u;
    pci_write16(bus, dev, fn, 0x04,
                (u16)(cmd & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_IO)));
    lo = pci_read32(bus, dev, fn, 0x10);
    hi = is64 ? pci_read32(bus, dev, fn, 0x14) : 0;
    pci_write32(bus, dev, fn, 0x10, 0xFFFFFFFFu);
    if (is64)
        pci_write32(bus, dev, fn, 0x14, 0xFFFFFFFFu);
    lo_mask = pci_read32(bus, dev, fn, 0x10);
    hi_mask = is64 ? pci_read32(bus, dev, fn, 0x14) : 0;
    pci_write32(bus, dev, fn, 0x10, lo);
    if (is64)
        pci_write32(bus, dev, fn, 0x14, hi);
    pci_write16(bus, dev, fn, 0x04, cmd);
    pci_enable(bus, dev, fn, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    mask = (u64)(lo_mask & ~0xFu);
    if (is64)
        mask |= (u64)hi_mask << 32;
    else
        mask |= 0xFFFFFFFF00000000ull;
    size = ~mask + 1ull;
    if (size < 0x1000ull || (size & (size - 1ull)) != 0)
        return -1;
    pa = (u64)(lo & ~0xFu) | ((u64)hi << 32);
    if (pa == 0)
        return -1;
    for (off = 0; off < size; off += CRUG_PAGE_SIZE) {
        if (vm_map_page(pa + off, pa + off,
                        VM_PRESENT | VM_WRITE | VM_NX | VM_PCD | VM_PWT |
                        VM_GLOBAL) != 0)
            return -1;
    }
    c->regs = (volatile u8 *)(uintptr)pa;
    c->bar_pa = pa;
    c->bar_size = size;
    return 0;
}

static int nvme_identify(struct nvme_ctrl *c, u32 nsid, u32 cns, u8 *out)
{
    u64 pa = pmm_alloc_page();
    int rc;

    if (!pa)
        return -1;
    rc = nvme_admin_cmd(c, NVME_ADMIN_IDENTIFY, nsid, cns, 0, pa);
    if (rc == 0)
        memcpy(out, pmm_phys_to_virt(pa), 4096);
    pmm_free_page(pa);
    return rc;
}

static int nvme_create_io_queues(struct nvme_ctrl *c)
{
    struct nvme_queue *q = &c->io;
    u32 qd = (u32)c->qsize - 1u;

    if (nvme_alloc_queue(c, q, 1, c->qsize) != 0)
        return -1;
    if (nvme_admin_cmd(c, NVME_ADMIN_CREATE_CQ, 0, (qd << 16) | 1u, 1,
                       q->cq_pa) != 0) {
        nvme_free_queue(q);
        return -1;
    }
    if (nvme_admin_cmd(c, NVME_ADMIN_CREATE_SQ, 0, (qd << 16) | 1u,
                       (1u << 16) | 1u, q->sq_pa) != 0) {
        nvme_free_queue(q);
        return -1;
    }
    return 0;
}

static int nvme_rw(struct block_device *bd, u64 lba, u32 count, void *buf,
                   int write)
{
    struct nvme_ctrl *c = (struct nvme_ctrl *)bd->priv;
    u64 bytes, pages, pa, list_pa = 0;
    u64 prp2 = 0;
    u64 i;
    int rc;

    if (!c || !c->ready)
        return -1;
    if (count == 0)
        return 0;
    if (lba + count > bd->sectors)
        return -1;
    bytes = (u64)count * 512ull;
    pages = (bytes + CRUG_PAGE_SIZE - 1ull) / CRUG_PAGE_SIZE;
    if (pages == 0 || pages > NVME_MAX_PAGES)
        return -1;
    pa = pmm_alloc_pages(pages);
    if (!pa)
        return -1;
    if (write)
        memcpy(pmm_phys_to_virt(pa), buf, bytes);
    if (pages == 2) {
        prp2 = pa + CRUG_PAGE_SIZE;
    } else if (pages > 2) {
        u64 *list;

        list_pa = pmm_alloc_page();
        if (!list_pa) {
            pmm_free_pages(pa, pages);
            return -1;
        }
        list = (u64 *)pmm_phys_to_virt(list_pa);
        for (i = 0; i + 1 < pages; i++)
            list[i] = pa + (i + 1) * CRUG_PAGE_SIZE;
        prp2 = list_pa;
    }
    rc = nvme_io_cmd(c, write ? NVME_IO_WRITE : NVME_IO_READ, lba,
                     count - 1u, pa, prp2);
    if (rc == 0 && !write)
        memcpy(buf, pmm_phys_to_virt(pa), bytes);
    if (list_pa)
        pmm_free_page(list_pa);
    pmm_free_pages(pa, pages);
    if (rc != 0) {
        c->ready = 0;
        return -1;
    }
    return 0;
}

static int nvme_read(struct block_device *bd, u64 lba, u32 count, void *buf)
{
    return nvme_rw(bd, lba, count, buf, 0);
}

static int nvme_write(struct block_device *bd, u64 lba, u32 count,
                      const void *buf)
{
    return nvme_rw(bd, lba, count, (void *)buf, 1);
}

static int nvme_flush(struct block_device *bd)
{
    struct nvme_ctrl *c = (struct nvme_ctrl *)bd->priv;

    if (!c || !c->ready)
        return -1;
    if (nvme_io_cmd(c, NVME_IO_FLUSH, 0, 0, 0, 0) != 0) {
        c->ready = 0;
        return -1;
    }
    return 0;
}

static int nvme_setup(struct nvme_ctrl *c, u8 bus, u8 dev, u8 fn)
{
    u64 cap, nsze;
    u32 mqes, qsize, lbaf, lbads;
    u8 nlbaf, flbas;

    memset(c, 0, sizeof(*c));
    if (nvme_bar_setup(c, bus, dev, fn) != 0)
        return -1;
    cap = nvme_read64(c, NVME_CAP);
    if (cap == 0 || cap == ~0ull)
        return -1;
    if (((cap >> 48) & 0xFu) != 0)
        return -1;
    mqes = (u32)(cap & 0xFFFFu);
    qsize = mqes + 1u;
    if (qsize > NVME_QUEUE_ENTRIES)
        qsize = NVME_QUEUE_ENTRIES;
    if (qsize < 2u)
        return -1;
    c->qsize = (u16)qsize;
    c->stride = 4u << ((u32)(cap >> 32) & 0xFu);

    nvme_write32(c, NVME_INTMS, 0xFFFFFFFFu);
    if (nvme_disable(c) != 0)
        return -1;
    if (nvme_alloc_queue(c, &c->admin, 0, c->qsize) != 0)
        return -1;
    nvme_write32(c, NVME_AQA, (qsize - 1u) | ((qsize - 1u) << 16));
    nvme_write64(c, NVME_ASQ, c->admin.sq_pa);
    nvme_write64(c, NVME_ACQ, c->admin.cq_pa);
    nvme_write32(c, NVME_CC, NVME_CC_CSS_NVM | NVME_CC_MPS_4K |
                  NVME_CC_IOSQES | NVME_CC_IOCQES | NVME_CC_EN);
    if (nvme_wait_ready(c, 1) != 0) {
        kprintf("nvme   : controller not ready\n");
        return -1;
    }
    if (nvme_read32(c, NVME_CSTS) & NVME_CSTS_CFS) {
        kprintf("nvme   : controller fatal\n");
        return -1;
    }
    if (nvme_identify(c, 0, 1, g_identify) != 0)
        return -1;
    if (nvme_identify(c, 1, 0, g_identify) != 0)
        return -1;
    nsze = *(u64 *)(g_identify + 0);
    nlbaf = g_identify[25];
    flbas = g_identify[26];
    if (nsze == 0 || (flbas & 0xFu) > nlbaf)
        return -1;
    lbaf = *(u32 *)(g_identify + 128 + 4 * (flbas & 0xFu));
    lbads = (lbaf >> 16) & 0xFFu;
    if (lbads != 9u) {
        kprintf("nvme   : unsupported lba size %u\n", lbads);
        return -1;
    }
    if (nvme_create_io_queues(c) != 0)
        return -1;
    c->nsid = 1;
    c->bd.sectors = nsze;
    c->bd.sector_size = 512;
    c->bd.read = nvme_read;
    c->bd.write = nvme_write;
    c->bd.flush = nvme_flush;
    c->bd.priv = c;
    c->ready = 1;
    return 0;
}

int nvme_init(void)
{
    u32 idx, found = 0;

    for (idx = 0; idx < NVME_MAX_CONTROLLERS; idx++) {
        u8 bus, dev, fn;
        struct nvme_ctrl *c = &g_ctrl[found];

        if (!pci_find_class_nth(NVME_PCI_CLASS, NVME_PCI_SUBCLASS,
                                NVME_PCI_PROG_IF, (int)idx, &bus, &dev,
                                &fn))
            break;
        if (nvme_setup(c, bus, dev, fn) != 0) {
            kprintf("nvme   : controller %u init failed\n", idx);
            continue;
        }
        ksnprintf(c->bd.name, sizeof(c->bd.name), "nvme%un1", found);
        if (block_register(&c->bd) != 0) {
            kprintf("nvme   : block register failed\n");
            continue;
        }
        kprintf("nvme   : %s %llu sectors, bar=0x%llx\n", c->bd.name,
                (unsigned long long)c->bd.sectors,
                (unsigned long long)c->bar_pa);
        found++;
    }
    g_count = found;
    return (int)found;
}

u32 nvme_device_count(void)
{
    return g_count;
}
