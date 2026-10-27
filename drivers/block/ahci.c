/* AHCI/SATA block driver: one polled command slot, LBA48 DMA transfers. */
#include <crug/ahci.h>
#include <crug/types.h>
#include <crug/compiler.h>
#include <crug/pci.h>
#include <crug/block.h>
#include <crug/mm.h>
#include <crug/vm.h>
#include <crug/timer.h>
#include <crug/string.h>
#include <crug/kernel.h>

#define AHCI_CAP        0x0000u
#define AHCI_GHC        0x0004u
#define AHCI_PI         0x000Cu
#define AHCI_PORT_BASE  0x0100u
#define AHCI_PORT_SIZE  0x0080u

#define AHCI_PORT_CLB   0x0000u
#define AHCI_PORT_CLBU  0x0004u
#define AHCI_PORT_FB    0x0008u
#define AHCI_PORT_FBU   0x000Cu
#define AHCI_PORT_IS    0x0010u
#define AHCI_PORT_CMD   0x0018u
#define AHCI_PORT_TFD   0x0020u
#define AHCI_PORT_SIG   0x0024u
#define AHCI_PORT_SSTS  0x0028u
#define AHCI_PORT_SERR  0x0030u
#define AHCI_PORT_CI    0x0038u

#define AHCI_GHC_AE     0x80000000u

#define AHCI_CMD_SUD    0x00000002u
#define AHCI_CMD_POD    0x00000004u
#define AHCI_CMD_FRE    0x00000010u
#define AHCI_CMD_ST     0x00000001u
#define AHCI_CMD_CR     0x00008000u
#define AHCI_CMD_FR     0x00004000u

#define AHCI_TFD_ERR    0x00000001u
#define AHCI_TFD_DRQ    0x00000008u
#define AHCI_TFD_BSY    0x00000080u

#define AHCI_SSTS_DET_MASK    0x0000000Fu
#define AHCI_SSTS_DET_PRESENT 0x00000003u
#define AHCI_SIG_ATA          0x00000101u

#define AHCI_CMD_IDENTIFY   0xECu
#define AHCI_CMD_READ_DMA   0x25u
#define AHCI_CMD_WRITE_DMA  0x35u
#define AHCI_CMD_FLUSH      0xEAu

#define AHCI_BOUNCE_BYTES 8192u
#define AHCI_MAX_SECTORS  (AHCI_BOUNCE_BYTES / CRUG_SECTOR_SIZE)
#define AHCI_TIMEOUT_MS   1000u
#define AHCI_LINK_TIMEOUT_MS 20u

struct ahci_prdt {
    u64 dba;
    u32 reserved;
    u32 dbc;
} __attribute__((packed));

struct ahci_cmd_table {
    u8 cfis[64];
    u8 acmd[32];
    u8 reserved[32];
    struct ahci_prdt prdt;
} __attribute__((packed));

struct ahci_cmd_header {
    u32 flags;
    u32 prdt_bytes;
    u64 ctba;
    u32 reserved[4];
} __attribute__((packed));

_Static_assert(sizeof(struct ahci_prdt) == 16, "AHCI PRDT entry must be 16 bytes");
_Static_assert(sizeof(struct ahci_cmd_table) == 144, "AHCI command table must be 144 bytes");
_Static_assert(sizeof(struct ahci_cmd_header) == 32, "AHCI command header must be 32 bytes");

struct ahci_ctrl {
    volatile u8 *regs;
    u64 bar_pa;
    u64 bar_size;
    u32 ports;
    u32 slots;
};

struct ahci_port {
    volatile u8 *regs;
    struct ahci_cmd_header *cmd;
    struct ahci_cmd_table *ct;
    u64 clb_pa;
    u64 fb_pa;
    u64 ct_pa;
    u32 index;
    struct block_device bd;
};

static struct ahci_ctrl g_ctrls[AHCI_MAX_CONTROLLERS];
static struct ahci_port g_ports[AHCI_MAX_DEVICES];
static u32 g_controller_count;
static u32 g_device_count;
static u8 g_bounce[AHCI_BOUNCE_BYTES] __attribute__((aligned(4096)));
static u8 g_identify[CRUG_SECTOR_SIZE] __attribute__((aligned(4096)));

static u32 ahci_read32(volatile u8 *base, u32 off)
{
    return *(volatile u32 *)(base + off);
}

static void ahci_write32(volatile u8 *base, u32 off, u32 value)
{
    *(volatile u32 *)(base + off) = value;
}

static void ahci_delay(void)
{
    __asm__ volatile("pause");
}

static u64 ahci_deadline(void)
{
    return timer_monotonic_ns() + (u64)AHCI_TIMEOUT_MS * 1000000ull;
}

static int ahci_port_stop(struct ahci_port *p)
{
    u64 deadline;
    u32 cmd;

    cmd = ahci_read32(p->regs, AHCI_PORT_CMD);
    cmd &= ~(AHCI_CMD_ST | AHCI_CMD_FRE);
    ahci_write32(p->regs, AHCI_PORT_CMD, cmd);
    deadline = ahci_deadline();
    while (ahci_read32(p->regs, AHCI_PORT_CMD) & (AHCI_CMD_CR | AHCI_CMD_FR)) {
        if (timer_monotonic_ns() > deadline)
            return -1;
        ahci_delay();
    }
    ahci_write32(p->regs, AHCI_PORT_SERR, 0xFFFFFFFFu);
    ahci_write32(p->regs, AHCI_PORT_IS, 0xFFFFFFFFu);
    return 0;
}

static void ahci_port_start(struct ahci_port *p)
{
    u32 cmd;

    ahci_write32(p->regs, AHCI_PORT_CLB, (u32)p->clb_pa);
    ahci_write32(p->regs, AHCI_PORT_CLBU, (u32)(p->clb_pa >> 32));
    ahci_write32(p->regs, AHCI_PORT_FB, (u32)p->fb_pa);
    ahci_write32(p->regs, AHCI_PORT_FBU, (u32)(p->fb_pa >> 32));
    cmd = ahci_read32(p->regs, AHCI_PORT_CMD);
    cmd |= AHCI_CMD_SUD | AHCI_CMD_POD | AHCI_CMD_FRE | AHCI_CMD_ST;
    ahci_write32(p->regs, AHCI_PORT_CMD, cmd);
}

static int ahci_wait_ready(struct ahci_port *p)
{
    u64 deadline = ahci_deadline();

    while (ahci_read32(p->regs, AHCI_PORT_TFD) & (AHCI_TFD_BSY | AHCI_TFD_DRQ)) {
        if (timer_monotonic_ns() > deadline)
            return -1;
        ahci_delay();
    }
    return 0;
}

static int ahci_exec(struct ahci_port *p, const u8 *fis, u32 fis_bytes,
                     int write, void *buf, u32 bytes)
{
    struct ahci_cmd_header *h;
    u32 prdtl = bytes ? 1u : 0u;
    u64 deadline;

    if (fis_bytes == 0 || fis_bytes > 64 || (fis_bytes & 3u) != 0)
        return -1;
    if (bytes > AHCI_BOUNCE_BYTES)
        return -1;
    if (ahci_wait_ready(p) != 0)
        return -1;
    memset(p->ct, 0, sizeof(*p->ct));
    memcpy(p->ct->cfis, fis, fis_bytes);
    if (bytes) {
        p->ct->prdt.dba = pmm_virt_to_phys(buf);
        p->ct->prdt.reserved = 0;
        p->ct->prdt.dbc = bytes - 1u;
    }
    h = p->cmd;
    memset(h, 0, sizeof(*h));
    h->flags = (fis_bytes / 4u) | (write ? (1u << 6) : 0u) | (prdtl << 16);
    h->prdt_bytes = prdtl * (u32)sizeof(struct ahci_prdt);
    h->ctba = p->ct_pa;
    ahci_write32(p->regs, AHCI_PORT_IS, 0xFFFFFFFFu);
    ahci_write32(p->regs, AHCI_PORT_CI, 1u);
    deadline = ahci_deadline();
    while (ahci_read32(p->regs, AHCI_PORT_CI) & 1u) {
        if (timer_monotonic_ns() > deadline)
            return -1;
        ahci_delay();
    }
    if (ahci_read32(p->regs, AHCI_PORT_TFD) & AHCI_TFD_ERR)
        return -1;
    if (ahci_read32(p->regs, AHCI_PORT_IS) & (1u << 30))
        return -1;
    return 0;
}

static void ahci_fis_h2d(u8 *fis, u8 command, u64 lba, u16 count)
{
    memset(fis, 0, 20);
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = command;
    fis[4] = (u8)(lba & 0xFFu);
    fis[5] = (u8)((lba >> 8) & 0xFFu);
    fis[6] = (u8)((lba >> 16) & 0xFFu);
    fis[7] = 0x40;
    fis[8] = (u8)((lba >> 24) & 0xFFu);
    fis[9] = (u8)((lba >> 32) & 0xFFu);
    fis[10] = (u8)((lba >> 40) & 0xFFu);
    fis[12] = (u8)(count & 0xFFu);
    fis[13] = (u8)((count >> 8) & 0xFFu);
}

static int ahci_transfer(struct ahci_port *p, u64 lba, u32 count, void *buf,
                         int write)
{
    u8 fis[20];

    ahci_fis_h2d(fis, write ? AHCI_CMD_WRITE_DMA : AHCI_CMD_READ_DMA, lba,
                 (u16)count);
    return ahci_exec(p, fis, sizeof(fis), write, buf,
                     count * (u32)CRUG_SECTOR_SIZE);
}

static int ahci_bd_read(struct block_device *bd, u64 lba, u32 count, void *buf)
{
    struct ahci_port *p = (struct ahci_port *)bd->priv;
    u8 *out = (u8 *)buf;

    while (count) {
        u32 chunk = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;

        if (ahci_transfer(p, lba, chunk, g_bounce, 0) != 0)
            return -1;
        memcpy(out, g_bounce, chunk * (u32)CRUG_SECTOR_SIZE);
        out += chunk * (u32)CRUG_SECTOR_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int ahci_bd_write(struct block_device *bd, u64 lba, u32 count,
                         const void *buf)
{
    struct ahci_port *p = (struct ahci_port *)bd->priv;
    const u8 *in = (const u8 *)buf;

    while (count) {
        u32 chunk = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;

        memcpy(g_bounce, in, chunk * (u32)CRUG_SECTOR_SIZE);
        if (ahci_transfer(p, lba, chunk, g_bounce, 1) != 0)
            return -1;
        in += chunk * (u32)CRUG_SECTOR_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int ahci_bd_flush(struct block_device *bd)
{
    struct ahci_port *p = (struct ahci_port *)bd->priv;
    u8 fis[20];

    ahci_fis_h2d(fis, AHCI_CMD_FLUSH, 0, 0);
    return ahci_exec(p, fis, sizeof(fis), 0, 0, 0);
}

static int ahci_bar_setup(struct ahci_ctrl *c, u8 bus, u8 dev, u8 fn)
{
    u32 bar = pci_read32(bus, dev, fn, 0x24);
    u16 cmd = pci_read16(bus, dev, fn, 0x04);
    u32 lo, hi, lo_mask, hi_mask;
    u64 mask, size, pa, off;
    int is64;

    if (bar & PCI_BAR_IO)
        return -1;
    is64 = ((bar >> 1) & 3u) == 2u;
    pci_write16(bus, dev, fn, 0x04,
                (u16)(cmd & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_IO)));
    lo = pci_read32(bus, dev, fn, 0x24);
    hi = is64 ? pci_read32(bus, dev, fn, 0x28) : 0;
    pci_write32(bus, dev, fn, 0x24, 0xFFFFFFFFu);
    if (is64)
        pci_write32(bus, dev, fn, 0x28, 0xFFFFFFFFu);
    lo_mask = pci_read32(bus, dev, fn, 0x24);
    hi_mask = is64 ? pci_read32(bus, dev, fn, 0x28) : 0;
    pci_write32(bus, dev, fn, 0x24, lo);
    if (is64)
        pci_write32(bus, dev, fn, 0x28, hi);
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
        u64 page = pa + off;
        u64 ucf = VM_PRESENT | VM_WRITE | VM_NX | VM_PCD | VM_PWT | VM_GLOBAL;

        /* The identity map may already cover the BAR; just make it
         * uncached in that case. */
        if (vm_translate(page) == page) {
            if (vm_protect_page(page, ucf) != 0)
                return -1;
        } else if (vm_map_page(page, page, ucf) != 0) {
            return -1;
        }
    }
    c->regs = (volatile u8 *)(uintptr)pa;
    c->bar_pa = pa;
    c->bar_size = size;
    return 0;
}

static int ahci_port_present(struct ahci_ctrl *c, u32 port)
{
    volatile u8 *regs;
    u32 ssts;
    u32 sig;
    u64 deadline;

    regs = c->regs + AHCI_PORT_BASE + port * AHCI_PORT_SIZE;
    /* Empty ports never report DET==3, so poll only briefly instead of
     * spending the full command timeout on every unpopulated port. */
    deadline = timer_monotonic_ns() + (u64)AHCI_LINK_TIMEOUT_MS * 1000000ull;
    for (;;) {
        ssts = ahci_read32(regs, AHCI_PORT_SSTS);
        if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_PRESENT)
            break;
        if (timer_monotonic_ns() > deadline)
            return 0;
        ahci_delay();
    }
    sig = ahci_read32(regs, AHCI_PORT_SIG);
    return sig == AHCI_SIG_ATA;
}

static int ahci_port_setup(struct ahci_ctrl *c, u32 port, struct ahci_port *p)
{
    u64 clb, fb, ct;

    memset(p, 0, sizeof(*p));
    p->regs = c->regs + AHCI_PORT_BASE + port * AHCI_PORT_SIZE;
    p->index = port;
    if (ahci_port_stop(p) != 0)
        return -1;
    clb = pmm_alloc_page();
    fb = pmm_alloc_page();
    ct = pmm_alloc_page();
    if (!clb || !fb || !ct) {
        if (clb)
            pmm_free_page(clb);
        if (fb)
            pmm_free_page(fb);
        if (ct)
            pmm_free_page(ct);
        return -1;
    }
    p->cmd = (struct ahci_cmd_header *)(uintptr)clb;
    p->ct = (struct ahci_cmd_table *)(uintptr)ct;
    p->clb_pa = clb;
    p->fb_pa = fb;
    p->ct_pa = ct;
    ahci_port_start(p);
    if (ahci_wait_ready(p) != 0) {
        ahci_port_stop(p);
        return -1;
    }
    return 0;
}

static u32 ahci_u16(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8);
}

static int ahci_port_register(struct ahci_ctrl *c, u32 port, struct ahci_port *p)
{
    const u8 *id = g_identify;
    u64 sectors = 0;
    u32 logical = CRUG_SECTOR_SIZE;
    u32 words;
    u8 fis[20];
    u32 slots;

    slots = c->slots;
    (void)slots;
    ahci_fis_h2d(fis, AHCI_CMD_IDENTIFY, 0, 0);
    memset(g_identify, 0, sizeof(g_identify));
    if (ahci_exec(p, fis, sizeof(fis), 0, g_identify, sizeof(g_identify)) != 0) {
        kprintf("ahci   : identify failed on port %u\n", port);
        return -1;
    }
    if (ahci_u16(id + 0) == 0 || ahci_u16(id + 0) == 0xFFFFu)
        return -1;
    if (ahci_u16(id + 166) & 0x0400u) {
        u32 i;

        for (i = 0; i < 4; i++)
            sectors |= (u64)ahci_u16(id + 200 + i * 2) << (16 * i);
    }
    if (sectors == 0) {
        sectors = (u64)ahci_u16(id + 120) |
                  ((u64)ahci_u16(id + 122) << 16);
    }
    if (sectors == 0) {
        kprintf("ahci   : no LBA capacity on port %u\n", port);
        return -1;
    }
    if (ahci_u16(id + 212) & 0x1000u) {
        words = ahci_u16(id + 234) | (ahci_u16(id + 236) << 16);
        if (words >= 256u && words <= 4096u && (words & (words - 1u)) == 0)
            logical = words;
    }
    if (logical != CRUG_SECTOR_SIZE) {
        kprintf("ahci   : unsupported logical sector size %u on port %u\n",
                logical, port);
        return -1;
    }
    memset(&p->bd, 0, sizeof(p->bd));
    p->bd.name[0] = 's';
    p->bd.name[1] = 'd';
    p->bd.name[2] = (char)('a' + (int)g_device_count);
    p->bd.sectors = sectors;
    p->bd.sector_size = CRUG_SECTOR_SIZE;
    p->bd.read = ahci_bd_read;
    p->bd.write = ahci_bd_write;
    p->bd.flush = ahci_bd_flush;
    p->bd.priv = p;
    if (block_register(&p->bd) != 0)
        return -1;
    g_device_count++;
    kprintf("ahci   : %s %llu sectors, port %u\n", p->bd.name,
            (unsigned long long)sectors, port);
    return 0;
}

static void ahci_controller_probe(struct ahci_ctrl *c)
{
    u32 cap, pi, ports, port;

    cap = ahci_read32(c->regs, AHCI_CAP);
    c->slots = ((cap >> 8) & 0x1Fu) + 1u;
    ports = (cap & 0x1Fu) + 1u;
    if (ports > 32u)
        ports = 32u;
    if (c->bar_size > AHCI_PORT_BASE) {
        u32 max_ports = (u32)((c->bar_size - AHCI_PORT_BASE) / AHCI_PORT_SIZE);
        if (ports > max_ports)
            ports = max_ports;
    }
    c->ports = ports;
    pi = ahci_read32(c->regs, AHCI_PI);
    for (port = 0; port < ports; port++) {
        struct ahci_port *p;

        if ((pi & (1u << port)) == 0)
            continue;
        if (g_device_count >= AHCI_MAX_DEVICES)
            break;
        if (!ahci_port_present(c, port))
            continue;
        p = &g_ports[g_device_count];
        if (ahci_port_setup(c, port, p) != 0) {
            kprintf("ahci   : port %u setup failed\n", port);
            continue;
        }
        if (ahci_port_register(c, port, p) != 0)
            ahci_port_stop(p);
    }
}

static int ahci_controller_setup(u8 bus, u8 dev, u8 fn, struct ahci_ctrl *c)
{
    u32 ghc;

    memset(c, 0, sizeof(*c));
    if (ahci_bar_setup(c, bus, dev, fn) != 0) {
        kprintf("ahci   : BAR setup failed at %02x:%02x.%u\n", bus, dev, fn);
        return -1;
    }
    ghc = ahci_read32(c->regs, AHCI_GHC);
    ahci_write32(c->regs, AHCI_GHC, ghc | AHCI_GHC_AE);
    ahci_controller_probe(c);
    return 0;
}

int ahci_init(void)
{
    int index;
    u8 bus, dev, fn;

    g_controller_count = 0;
    g_device_count = 0;
    for (index = 0; index < (int)AHCI_MAX_CONTROLLERS; index++) {
        if (pci_find_class_nth(AHCI_PCI_CLASS, AHCI_PCI_SUBCLASS,
                               AHCI_PCI_PROG_IF, index, &bus, &dev, &fn) == 0)
            break;
        if (ahci_controller_setup(bus, dev, fn,
                                  &g_ctrls[g_controller_count]) == 0)
            g_controller_count++;
    }
    return (int)g_device_count;
}

u32 ahci_device_count(void)
{
    return g_device_count;
}

