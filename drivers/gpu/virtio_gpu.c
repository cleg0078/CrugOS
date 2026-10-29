/* Phase 8A: virtio-gpu 2D scanout over the modern virtio-pci transport.
   QEMU's virtio-gpu-pci is a modern-only PCIe function, so the driver walks
   the vendor capabilities, maps the common/notify/device BARs and drives one
   split control queue (64 entries).

   The driver owns one B8G8R8X8 resource backed by contiguous guest pages and
   sets it as scanout 0; the backing buffer becomes the linear framebuffer.
   Every present is one TRANSFER_TO_HOST_2D plus one RESOURCE_FLUSH for the
   damaged rectangle, so the Workbench compositor is rasterised by the CPU
   into GPU memory and only the changed pixels cross the device boundary. */
#include <crug/gpu.h>
#include <crug/pci.h>
#include <crug/io.h>
#include <crug/mm.h>
#include <crug/vm.h>
#include <crug/timer.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include <crug/fb.h>
#include <crug/virtio.h>

#define GPU_PCI_LEGACY 0x1010u
#define GPU_PCI_MODERN 0x1050u
#define GPU_QUEUE_MAX 64u
#define GPU_QUEUE_PAGES 3u
#define GPU_CMD_MAX 256u
#define GPU_RESP_MAX 512u
#define GPU_TIMEOUT_NS 2000000000ull
#define GPU_FORMAT_B8G8R8X8 2u
#define GPU_RESOURCE_ID 1u

#define GPU_CMD_GET_DISPLAY_INFO 0x0100u
#define GPU_CMD_RESOURCE_CREATE_2D 0x0101u
#define GPU_CMD_SET_SCANOUT 0x0103u
#define GPU_CMD_RESOURCE_FLUSH 0x0104u
#define GPU_CMD_TRANSFER_TO_HOST_2D 0x0105u
#define GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define GPU_RESP_OK_NODATA 0x1100u
#define GPU_RESP_OK_DISPLAY_INFO 0x1101u

#define GPU_VQ_F_NEXT 1u
#define GPU_VQ_F_WRITE 2u

/* Modern virtio-pci common configuration (virtio 1.0 4.1.4.3). */
#define GPU_CFG_DEVICE_FEATURE_SELECT 0x00u
#define GPU_CFG_DEVICE_FEATURE 0x04u
#define GPU_CFG_DRIVER_FEATURE_SELECT 0x08u
#define GPU_CFG_DRIVER_FEATURE 0x0Cu
#define GPU_CFG_DEVICE_STATUS 0x14u
#define GPU_CFG_QUEUE_SELECT 0x16u
#define GPU_CFG_QUEUE_SIZE 0x18u
#define GPU_CFG_QUEUE_ENABLE 0x1Cu
#define GPU_CFG_QUEUE_NOTIFY_OFF 0x1Eu
#define GPU_CFG_QUEUE_DESC 0x20u
#define GPU_CFG_QUEUE_DRIVER 0x28u
#define GPU_CFG_QUEUE_DEVICE 0x30u

#define GPU_PCI_CAP_VENDOR 0x09u
#define GPU_PCI_CAP_COMMON 1u
#define GPU_PCI_CAP_NOTIFY 2u
#define GPU_PCI_CAP_ISR 3u
#define GPU_PCI_CAP_DEVICE 4u

struct gpu_vq_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} __attribute__((packed));

struct gpu_vq_avail {
    u16 flags;
    u16 idx;
    u16 ring[GPU_QUEUE_MAX];
    u16 used_event;
} __attribute__((packed));

struct gpu_vq_used_elem {
    u32 id;
    u32 len;
} __attribute__((packed));

struct gpu_vq_used {
    u16 flags;
    u16 idx;
    struct gpu_vq_used_elem ring[GPU_QUEUE_MAX];
    u16 avail_event;
} __attribute__((packed));

struct gpu_caps {
    u8 common_bar;
    u8 notify_bar;
    u8 isr_bar;
    u8 device_bar;
    u32 common_off;
    u32 notify_off;
    u32 isr_off;
    u32 device_off;
    u32 notify_mul;
    int have_common;
    int have_notify;
};

struct gpu_state {
    int present;
    int enabled;
    u8 bus;
    u8 dev;
    u8 fn;
    struct gpu_caps caps;
    volatile u8 *bar_va[6];
    u64 bar_size[6];
    volatile u8 *common;
    volatile u8 *notify;
    volatile u8 *isr;
    u16 qsize;
    u64 queue_pa;
    struct gpu_vq_desc *desc;
    struct gpu_vq_avail *avail;
    struct gpu_vq_used *used;
    u16 notify_off;
    u16 last_used;
    u32 width;
    u32 height;
    u64 backing_pa;
    u64 backing_pages;
    u8 *backing;
    u64 transfers;
    u64 flushes;
    u32 errors;
};

static struct gpu_state g_gpu;
static u8 g_cmd[GPU_CMD_MAX] __attribute__((aligned(64)));
static u8 g_resp[GPU_RESP_MAX] __attribute__((aligned(64)));
static int g_gpu_enable = 1;
static u32 g_gpu_want_w;
static u32 g_gpu_want_h;
static int g_gpu_test;

static int gpu_is_space(char c)
{
    return c == ' ' || c == 0x09 || c == 0x0A || c == 0x0D;
}

static int gpu_prefix(const char *tok, u32 len, const char *prefix)
{
    u32 i = 0;

    while (prefix[i]) {
        if (i >= len || tok[i] != prefix[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

static u32 gpu_parse_u32(const char *text, u32 len, u32 max)
{
    u32 i = 0;
    u32 value = 0;

    while (i < len && text[i] >= '0' && text[i] <= '9') {
        value = value * 10u + (u32)(text[i] - '0');
        if (value > max) {
            return max;
        }
        i++;
    }
    return i ? value : 0u;
}

static void gpu_parse_size(const char *text, u32 len)
{
    u32 i = 0;
    u32 w;
    u32 h;

    w = gpu_parse_u32(text, len, GPU_RECT_MAX_W);
    while (i < len && text[i] >= '0' && text[i] <= '9') {
        i++;
    }
    if (i >= len || (text[i] != 'x' && text[i] != 'X')) {
        return;
    }
    i++;
    h = gpu_parse_u32(text + i, len - i, GPU_RECT_MAX_H);
    if (w && h) {
        g_gpu_want_w = w;
        g_gpu_want_h = h;
    }
}

/* The virtio-pci modern common configuration only latches a field written
   with the field own width: a 64-bit queue address written as byte stores is
   silently dropped by the device, leaving desc/avail/used at zero. Every
   accessor below therefore performs exactly one naturally sized access. */
static u8 gpu_r8(volatile u8 *base, u32 off)
{
    return *(volatile u8 *)(uintptr)((u8 *)base + off);
}

static void gpu_w8(volatile u8 *base, u32 off, u8 value)
{
    *(volatile u8 *)(uintptr)((u8 *)base + off) = value;
}

static u16 gpu_r16(volatile u8 *base, u32 off)
{
    return *(volatile u16 *)(uintptr)((u8 *)base + off);
}

static void gpu_w16(volatile u8 *base, u32 off, u16 value)
{
    *(volatile u16 *)(uintptr)((u8 *)base + off) = value;
}

static u32 gpu_r32(volatile u8 *base, u32 off)
{
    return *(volatile u32 *)(uintptr)((u8 *)base + off);
}

static void gpu_w32(volatile u8 *base, u32 off, u32 value)
{
    *(volatile u32 *)(uintptr)((u8 *)base + off) = value;
}

static void gpu_w64(volatile u8 *base, u32 off, u64 value)
{
    *(volatile u64 *)(uintptr)((u8 *)base + off) = value;
}

static u8 gpu_status_read(struct gpu_state *g)
{
    return gpu_r8(g->common, GPU_CFG_DEVICE_STATUS);
}

static void gpu_status_write(struct gpu_state *g, u8 status)
{
    gpu_w8(g->common, GPU_CFG_DEVICE_STATUS, status);
}

static void gpu_cmdline_token(const char *tok, u32 len)
{
    if (gpu_prefix(tok, len, "gpu.enable=")) {
        g_gpu_enable = gpu_parse_u32(tok + 11, len - 11u, 1u) ? 1 : 0;
    } else if (gpu_prefix(tok, len, "gpu.size=")) {
        gpu_parse_size(tok + 9, len - 9u);
    } else if (gpu_prefix(tok, len, "gpu.test=")) {
        g_gpu_test = gpu_parse_u32(tok + 9, len - 9u, 1u) ? 1 : 0;
    }
}

static int gpu_map_bar(struct gpu_state *g, u8 bar, volatile u8 **out_va,
                       u64 *out_size)
{
    u16 off;
    u32 bar0;
    u16 cmd;
    int is64;
    u32 lo;
    u32 hi;
    u32 lo_mask;
    u32 hi_mask;
    u64 mask;
    u64 size;
    u64 pa;
    u64 delta;

    if (bar > 5u || g->bar_va[bar]) {
        *out_va = g->bar_va[bar];
        *out_size = g->bar_size[bar];
        return *out_va ? 0 : -1;
    }
    off = (u16)(0x10u + (u16)bar * 4u);
    bar0 = pci_read32(g->bus, g->dev, g->fn, off);
    if (bar0 & PCI_BAR_IO) {
        return -1;
    }
    is64 = ((bar0 >> 1) & 3u) == 2u;
    if (is64 && bar == 5u) {
        return -1;
    }
    cmd = pci_read16(g->bus, g->dev, g->fn, 0x04);
    pci_write16(g->bus, g->dev, g->fn, 0x04,
                (u16)(cmd & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_IO)));
    lo = pci_read32(g->bus, g->dev, g->fn, off);
    hi = is64 ? pci_read32(g->bus, g->dev, g->fn, (u16)(off + 4u)) : 0u;
    pci_write32(g->bus, g->dev, g->fn, off, 0xFFFFFFFFu);
    if (is64) {
        pci_write32(g->bus, g->dev, g->fn, (u16)(off + 4u), 0xFFFFFFFFu);
    }
    lo_mask = pci_read32(g->bus, g->dev, g->fn, off);
    hi_mask = is64 ? pci_read32(g->bus, g->dev, g->fn, (u16)(off + 4u)) : 0u;
    pci_write32(g->bus, g->dev, g->fn, off, lo);
    if (is64) {
        pci_write32(g->bus, g->dev, g->fn, (u16)(off + 4u), hi);
    }
    pci_write16(g->bus, g->dev, g->fn, 0x04, cmd);
    pci_enable(g->bus, g->dev, g->fn, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    mask = (u64)(lo_mask & ~0xFu);
    if (is64) {
        mask |= (u64)hi_mask << 32;
    } else {
        mask |= 0xFFFFFFFF00000000ull;
    }
    size = ~mask + 1ull;
    if (size < 0x1000ull || (size & (size - 1ull)) != 0ull) {
        return -1;
    }
    pa = (u64)(lo & ~0xFu) | ((u64)hi << 32);
    if (!pa) {
        return -1;
    }
    for (delta = 0; delta < size; delta += CRUG_PAGE_SIZE) {
        if (vm_map_page(pa + delta, pa + delta,
                        VM_PRESENT | VM_WRITE | VM_NX | VM_PCD | VM_PWT |
                        VM_GLOBAL) != 0) {
            return -1;
        }
    }
    g->bar_va[bar] = (volatile u8 *)(uintptr)pa;
    g->bar_size[bar] = size;
    *out_va = g->bar_va[bar];
    *out_size = size;
    return 0;
}

static int gpu_parse_caps(struct gpu_state *g)
{
    u16 status = pci_read16(g->bus, g->dev, g->fn, 0x06);
    u8 cap;
    u32 guard = 0u;

    if (!(status & 0x0010u)) {
        return -1;
    }
    cap = (u8)(pci_read8(g->bus, g->dev, g->fn, 0x34) & 0xFCu);
    while (cap && guard++ < 64u) {
        u8 id = pci_read8(g->bus, g->dev, g->fn, cap);
        u8 next = (u8)(pci_read8(g->bus, g->dev, g->fn, (u16)(cap + 1u)) & 0xFCu);
        u8 len = pci_read8(g->bus, g->dev, g->fn, (u16)(cap + 2u));

        if (id == GPU_PCI_CAP_VENDOR && len >= 16u) {
            u8 type = pci_read8(g->bus, g->dev, g->fn, (u16)(cap + 3u));
            u8 bar = pci_read8(g->bus, g->dev, g->fn, (u16)(cap + 4u));
            u32 offset = pci_read32(g->bus, g->dev, g->fn, (u16)(cap + 8u));

            switch (type) {
            case GPU_PCI_CAP_COMMON:
                g->caps.common_bar = bar;
                g->caps.common_off = offset;
                g->caps.have_common = 1;
                break;
            case GPU_PCI_CAP_NOTIFY:
                g->caps.notify_bar = bar;
                g->caps.notify_off = offset;
                g->caps.notify_mul = (len >= 20u)
                                         ? pci_read32(g->bus, g->dev, g->fn,
                                                      (u16)(cap + 16u))
                                         : 0u;
                g->caps.have_notify = 1;
                break;
            case GPU_PCI_CAP_ISR:
                g->caps.isr_bar = bar;
                g->caps.isr_off = offset;
                break;
            case GPU_PCI_CAP_DEVICE:
                g->caps.device_bar = bar;
                g->caps.device_off = offset;
                break;
            default:
                break;
            }
        }
        cap = next;
    }
    return (g->caps.have_common && g->caps.have_notify) ? 0 : -1;
}

static int gpu_queue_init(struct gpu_state *g)
{
    volatile u8 *va;
    u64 size;
    u16 qsize;
    u64 pa;

    gpu_w16(g->common, GPU_CFG_QUEUE_SELECT, 0u);
    qsize = gpu_r16(g->common, GPU_CFG_QUEUE_SIZE);
    if (qsize < 2u || qsize > GPU_QUEUE_MAX || (qsize & (qsize - 1u)) != 0u) {
        kprintf("CRUGOS: virtio-gpu unsupported queue size %u\n", (unsigned)qsize);
        return -1;
    }
    pa = pmm_alloc_pages(GPU_QUEUE_PAGES);
    if (!pa) {
        return -1;
    }
    memset(pmm_phys_to_virt(pa), 0, GPU_QUEUE_PAGES * CRUG_PAGE_SIZE);
    g->qsize = qsize;
    g->queue_pa = pa;
    g->desc = (struct gpu_vq_desc *)pmm_phys_to_virt(pa);
    g->avail = (struct gpu_vq_avail *)pmm_phys_to_virt(pa + CRUG_PAGE_SIZE);
    g->used = (struct gpu_vq_used *)pmm_phys_to_virt(pa + 2u * CRUG_PAGE_SIZE);
    g->last_used = 0u;
    g->notify_off = gpu_r16(g->common, GPU_CFG_QUEUE_NOTIFY_OFF);
    gpu_w64(g->common, GPU_CFG_QUEUE_DESC, pa);
    gpu_w64(g->common, GPU_CFG_QUEUE_DRIVER, pa + CRUG_PAGE_SIZE);
    gpu_w64(g->common, GPU_CFG_QUEUE_DEVICE, pa + 2u * CRUG_PAGE_SIZE);
    gpu_w16(g->common, GPU_CFG_QUEUE_ENABLE, 1u);
    if (!g->bar_va[g->caps.notify_bar]) {
        if (gpu_map_bar(g, g->caps.notify_bar, &va, &size) != 0) {
            return -1;
        }
    }
    g->notify = g->bar_va[g->caps.notify_bar] + g->caps.notify_off;
    if (g->caps.isr_bar < 6u && gpu_map_bar(g, g->caps.isr_bar, &va, &size) == 0) {
        g->isr = g->bar_va[g->caps.isr_bar] + g->caps.isr_off;
    }
    return 0;
}

static void gpu_notify(struct gpu_state *g)
{
    /* Control queue 0: notification area offset = queue_index * multiplier.
       The queue index must be a single 16-bit store: QEMU ignores the
       notification when the access is split into byte writes. */
    *(volatile u16 *)(uintptr)g->notify = 0u;
    __sync_synchronize();
}

static void gpu_present_cb(u32 x, u32 y, u32 w, u32 h);

static int gpu_submit(u32 cmd_len, u32 want_type)
{
    struct gpu_state *g = &g_gpu;
    u16 head;
    u64 deadline;
    u32 type;

    if (!g->present || cmd_len < 24u || cmd_len > GPU_CMD_MAX) {
        return -1;
    }
    memset(g_resp, 0, GPU_RESP_MAX);
    g->desc[0].addr = pmm_virt_to_phys(g_cmd);
    g->desc[0].len = cmd_len;
    g->desc[0].flags = GPU_VQ_F_NEXT;
    g->desc[0].next = 1u;
    g->desc[1].addr = pmm_virt_to_phys(g_resp);
    g->desc[1].len = GPU_RESP_MAX;
    g->desc[1].flags = GPU_VQ_F_WRITE;
    g->desc[1].next = 0u;

    head = (u16)(g->avail->idx % g->qsize);
    g->avail->ring[head] = 0u;
    __sync_synchronize();
    g->avail->idx++;
    __sync_synchronize();
    gpu_notify(g);

    deadline = timer_monotonic_ns() + GPU_TIMEOUT_NS;
    while (g->used->idx == g->last_used) {
        if (timer_monotonic_ns() > deadline) {
            g->errors++;
            return -1;
        }
    }
    __sync_synchronize();
    g->last_used = g->used->idx;
    type = gpu_get32(g_resp, 0u);
    if (want_type && type != want_type) {
        kprintf("CRUGOS: virtio-gpu command 0x%x answered 0x%x (%s)\n",
                (unsigned)cmd_len, (unsigned)type, gpu_resp_name(type));
        g->errors++;
        return -1;
    }
    if (!want_type && gpu_resp_class(type) <= 0) {
        g->errors++;
        return -1;
    }
    return 0;
}

static void gpu_fill_rect_cmd(u32 off, u32 x, u32 y, u32 w, u32 h)
{
    gpu_put32(g_cmd, off + 0u, x);
    gpu_put32(g_cmd, off + 4u, y);
    gpu_put32(g_cmd, off + 8u, w);
    gpu_put32(g_cmd, off + 12u, h);
}

static int gpu_display_info(struct gpu_state *g, u32 *width, u32 *height)
{
    gpu_cmd_init(g_cmd, GPU_CMD_MAX, GPU_CMD_GET_DISPLAY_INFO);
    if (gpu_submit(24u, GPU_RESP_OK_DISPLAY_INFO) != 0) {
        return -1;
    }
    return gpu_display_pick(g_resp, GPU_RESP_MAX, g_gpu_want_w, g_gpu_want_h,
                            width, height);
}

static int gpu_create_resource(struct gpu_state *g, u32 width, u32 height)
{
    gpu_cmd_init(g_cmd, GPU_CMD_MAX, GPU_CMD_RESOURCE_CREATE_2D);
    gpu_put32(g_cmd, 24u, GPU_RESOURCE_ID);
    gpu_put32(g_cmd, 28u, GPU_FORMAT_B8G8R8X8);
    gpu_put32(g_cmd, 32u, width);
    gpu_put32(g_cmd, 36u, height);
    return gpu_submit(40u, GPU_RESP_OK_NODATA);
}

static int gpu_attach_backing(struct gpu_state *g)
{
    u32 len = (u32)(g->backing_pages * CRUG_PAGE_SIZE);

    gpu_cmd_init(g_cmd, GPU_CMD_MAX, GPU_CMD_RESOURCE_ATTACH_BACKING);
    gpu_put32(g_cmd, 24u, GPU_RESOURCE_ID);
    gpu_put32(g_cmd, 28u, 1u);
    gpu_put64(g_cmd, 32u, g->backing_pa);
    gpu_put32(g_cmd, 40u, len);
    gpu_put32(g_cmd, 44u, 0u);
    return gpu_submit(48u, GPU_RESP_OK_NODATA);
}

static int gpu_set_scanout(struct gpu_state *g)
{
    gpu_cmd_init(g_cmd, GPU_CMD_MAX, GPU_CMD_SET_SCANOUT);
    gpu_fill_rect_cmd(24u, 0u, 0u, g->width, g->height);
    gpu_put32(g_cmd, 40u, 0u);
    gpu_put32(g_cmd, 44u, GPU_RESOURCE_ID);
    return gpu_submit(48u, GPU_RESP_OK_NODATA);
}

int gpu_present(u32 x, u32 y, u32 w, u32 h)
{
    struct gpu_state *g = &g_gpu;
    struct gpu_rect r;
    u64 offset;

    if (!g->enabled || !g->backing) {
        return -1;
    }
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    if (!gpu_rect_clip(&r, g->width, g->height)) {
        return 0;
    }
    offset = (u64)r.y * (u64)g->width * 4u + (u64)r.x * 4u;

    gpu_cmd_init(g_cmd, GPU_CMD_MAX, GPU_CMD_TRANSFER_TO_HOST_2D);
    gpu_fill_rect_cmd(24u, r.x, r.y, r.w, r.h);
    gpu_put64(g_cmd, 40u, offset);
    gpu_put32(g_cmd, 48u, GPU_RESOURCE_ID);
    gpu_put32(g_cmd, 52u, 0u);
    if (gpu_submit(56u, GPU_RESP_OK_NODATA) != 0) {
        return -1;
    }
    g->transfers++;

    gpu_cmd_init(g_cmd, GPU_CMD_MAX, GPU_CMD_RESOURCE_FLUSH);
    gpu_fill_rect_cmd(24u, r.x, r.y, r.w, r.h);
    gpu_put32(g_cmd, 40u, GPU_RESOURCE_ID);
    gpu_put32(g_cmd, 44u, 0u);
    if (gpu_submit(48u, GPU_RESP_OK_NODATA) != 0) {
        return -1;
    }
    g->flushes++;
    return 0;
}

static void gpu_present_cb(u32 x, u32 y, u32 w, u32 h)
{
    (void)gpu_present(x, y, w, h);
}

/* Sizes are tried in order until a contiguous backing buffer fits: the
   preferred gpu.size=, the display mode, then 1024x768, 800x600 and 640x480. */
static int gpu_pick_backing(u32 display_w, u32 display_h, u32 *out_w,
                            u32 *out_h)
{
    static const u32 fallback[][2] = { { 1024u, 768u }, { 800u, 600u },
                                       { 640u, 480u } };
    u32 widths[4];
    u32 heights[4];
    u32 count = 0u;
    u32 i;

    if (g_gpu_want_w && g_gpu_want_h) {
        widths[count] = g_gpu_want_w;
        heights[count] = g_gpu_want_h;
        count++;
    }
    if (gpu_backing_pages(display_w, display_h) &&
        !(display_w == g_gpu_want_w && display_h == g_gpu_want_h)) {
        widths[count] = display_w;
        heights[count] = display_h;
        count++;
    }
    for (i = 0; i < 3u && count < 4u; i++) {
        if (fallback[i][0] == display_w && fallback[i][1] == display_h) {
            continue;
        }
        widths[count] = fallback[i][0];
        heights[count] = fallback[i][1];
        count++;
    }
    for (i = 0; i < count; i++) {
        u32 pages = gpu_backing_pages(widths[i], heights[i]);
        u64 pa;

        if (!pages) {
            continue;
        }
        pa = pmm_alloc_pages(pages);
        if (!pa) {
            continue;
        }
        g_gpu.backing_pa = pa;
        g_gpu.backing_pages = pages;
        g_gpu.backing = (u8 *)pmm_phys_to_virt(pa);
        *out_w = widths[i];
        *out_h = heights[i];
        return 0;
    }
    return -1;
}

int gpu_init(void)
{
    struct gpu_state *g = &g_gpu;
    volatile u8 *va;
    u64 size;
    u32 display_w = 0u;
    u32 display_h = 0u;
    u32 width = 0u;
    u32 height = 0u;
    u32 features_hi;

    g->present = 0;
    g->enabled = 0;
    if (!g_gpu_enable) {
        kprintf("CRUGOS: virtio-gpu disabled (gpu.enable=0), software framebuffer\n");
        return 0;
    }
    if (!pci_find(VIRTIO_PCI_VENDOR, GPU_PCI_MODERN, &g->bus, &g->dev, &g->fn)) {
        if (pci_find(VIRTIO_PCI_VENDOR, GPU_PCI_LEGACY, &g->bus, &g->dev, &g->fn)) {
            kprintf("CRUGOS: virtio-gpu ignored (legacy-only device, modern transport required)\n");
        }
        return 0;
    }
    pci_enable(g->bus, g->dev, g->fn, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    if (gpu_parse_caps(g) != 0) {
        kprintf("CRUGOS: virtio-gpu ignored (no virtio capabilities)\n");
        return 0;
    }
    if (gpu_map_bar(g, g->caps.common_bar, &va, &size) != 0) {
        kprintf("CRUGOS: virtio-gpu ignored (common BAR unmapable)\n");
        return 0;
    }
    g->common = va + g->caps.common_off;

    gpu_status_write(g, 0u);
    gpu_status_write(g, VIRTIO_STATUS_ACKNOWLEDGE);
    gpu_status_write(g, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    gpu_w32(g->common, GPU_CFG_DEVICE_FEATURE_SELECT, 0u);
    (void)gpu_r32(g->common, GPU_CFG_DEVICE_FEATURE);
    gpu_w32(g->common, GPU_CFG_DEVICE_FEATURE_SELECT, 1u);
    features_hi = gpu_r32(g->common, GPU_CFG_DEVICE_FEATURE);
    gpu_w32(g->common, GPU_CFG_DRIVER_FEATURE_SELECT, 0u);
    gpu_w32(g->common, GPU_CFG_DRIVER_FEATURE, 0u);
    gpu_w32(g->common, GPU_CFG_DRIVER_FEATURE_SELECT, 1u);
    gpu_w32(g->common, GPU_CFG_DRIVER_FEATURE, (features_hi & 1u) ? 1u : 0u);
    gpu_status_write(g, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                        VIRTIO_STATUS_FEATURES_OK);
    if (!(gpu_status_read(g) & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("CRUGOS: virtio-gpu ignored (features rejected)\n");
        return 0;
    }
    g->present = 1;
    if (gpu_queue_init(g) != 0) {
        g->present = 0;
        kprintf("CRUGOS: virtio-gpu ignored (no control queue)\n");
        return 0;
    }
    gpu_status_write(g, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                        VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    if (gpu_display_info(g, &display_w, &display_h) != 0) {
        kprintf("CRUGOS: virtio-gpu ignored (no display mode)\n");
        g->present = 0;
        return 0;
    }
    if (gpu_pick_backing(display_w, display_h, &width, &height) != 0) {
        kprintf("CRUGOS: virtio-gpu ignored (no backing memory for %ux%u)\n",
                (unsigned)display_w, (unsigned)display_h);
        g->present = 0;
        return 0;
    }
    g->width = width;
    g->height = height;
    if (gpu_create_resource(g, width, height) != 0) {
        kprintf("CRUGOS: virtio-gpu ignored (resource create failed)\n");
        g->present = 0;
        return 0;
    }
    if (gpu_attach_backing(g) != 0) {
        kprintf("CRUGOS: virtio-gpu ignored (attach backing failed)\n");
        g->present = 0;
        return 0;
    }
    if (gpu_set_scanout(g) != 0) {
        kprintf("CRUGOS: virtio-gpu ignored (set scanout failed)\n");
        g->present = 0;
        return 0;
    }
    memset(g->backing, 0, (usize)g->backing_pages * CRUG_PAGE_SIZE);
    if (fb_attach_linear(g->backing, width, height, width, CRUG_PIXEL_BGRX8888,
                         gpu_present_cb) != 0) {
        kprintf("CRUGOS: virtio-gpu ignored (framebuffer attach failed)\n");
        g->present = 0;
        return 0;
    }
    g->enabled = 1;
    kprintf("CRUGOS: virtio-gpu ready (%ux%u, %llu backing page(s), resource %u, modern pci)\n",
            (unsigned)width, (unsigned)height,
            (unsigned long long)g->backing_pages, (unsigned)GPU_RESOURCE_ID);
    kprintf("CRUGOS: gpu scanout ok (%ux%u, %llu transfer(s), %llu flush(es))\n",
            (unsigned)width, (unsigned)height,
            (unsigned long long)g->transfers, (unsigned long long)g->flushes);
    return 0;
}

void gpu_cmdline(const boot_info *bi)
{
    const char *cmd = bi ? bi->command_line : 0;
    u64 len = bi ? bi->command_line_len : 0;
    u64 i = 0;

    while (cmd && i < len) {
        u64 start;

        while (i < len && gpu_is_space(cmd[i])) {
            i++;
        }
        if (i >= len) {
            break;
        }
        start = i;
        while (i < len && !gpu_is_space(cmd[i])) {
            i++;
        }
        gpu_cmdline_token(cmd + start, (u32)(i - start));
    }
}

int gpu_ready(void)
{
    return g_gpu.enabled;
}

u32 gpu_width(void)
{
    return g_gpu.width;
}

u32 gpu_height(void)
{
    return g_gpu.height;
}

u64 gpu_transfers(void)
{
    return g_gpu.transfers;
}

u64 gpu_flushes(void)
{
    return g_gpu.flushes;
}

u32 gpu_errors(void)
{
    return g_gpu.errors;
}

/* Re-present the whole resource: with a GPU attached the desktop pixels
   leave the guest through the same path the compositor uses. */
int gpu_selftest(void)
{
    struct gpu_state *g = &g_gpu;

    if (!g->enabled) {
        if (g_gpu_test) {
            kprintf("CRUGOS: gpu selftest skipped (software framebuffer)\n");
        }
        return 0;
    }
    kprintf("CRUGOS: gpu selftest start\n");
    if (gpu_present(0u, 0u, g->width, g->height) != 0) {
        kprintf("CRUGOS: gpu selftest failed (present)\n");
        return -1;
    }
    kprintf("CRUGOS: gpu selftest ok (%ux%u, %llu transfer(s), %llu flush(es))\n",
            (unsigned)g->width, (unsigned)g->height,
            (unsigned long long)g->transfers, (unsigned long long)g->flushes);
    return 0;
}
