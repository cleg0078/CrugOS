/* xHCI host controller: command/event rings, port resets and HID boot keyboard.

   Everything is polled: no interrupts are enabled and no MSI vectors are
   allocated. The controller is reset on attach, then the first boot-protocol
   keyboard found on any port is enumerated and its interrupt IN endpoint is
   armed with a chain of NORMAL transfer requests. */
#include <crug/types.h>
#include <crug/usb.h>
#include <crug/pci.h>
#include <crug/mm.h>
#include <crug/vm.h>
#include <crug/kernel.h>
#include <crug/input.h>
#include <crug/timer.h>
#include <crug/cpu.h>
#include <crug/string.h>
#include <crug/bootinfo.h>

/* Capability registers. */
#define XHCI_CAP_CAPLENGTH   0x00u
#define XHCI_CAP_HCSPARAMS1  0x04u
#define XHCI_CAP_HCCPARAMS1  0x10u
#define XHCI_CAP_DBOFF       0x14u
#define XHCI_CAP_RTSOFF      0x18u

/* Operational registers. */
#define XHCI_OP_USBCMD  0x00u
#define XHCI_OP_USBSTS  0x04u
#define XHCI_OP_CRCR    0x18u
#define XHCI_OP_DCBAAP  0x30u
#define XHCI_OP_CONFIG  0x38u
#define XHCI_OP_PORTSC  0x400u
#define XHCI_PORT_STRIDE 0x10u

#define XHCI_CMD_RUN    (1u << 0)
#define XHCI_CMD_HCRST  (1u << 1)
#define XHCI_STS_HALT   (1u << 0)
#define XHCI_STS_CNR    (1u << 11)

/* Interrupter 0: the runtime register block starts with the 0x20-byte
   interrupter register set. */
#define XHCI_RT_IMAN    (0x20u + 0x00u)
#define XHCI_RT_ERSTSZ  (0x20u + 0x08u)
#define XHCI_RT_ERSTBA  (0x20u + 0x10u)
#define XHCI_RT_ERDP    (0x20u + 0x18u)
#define XHCI_ERDP_EHB   (1u << 3)

/* PORTSC bits. */
#define PORT_CCS  (1u << 0)
#define PORT_PED  (1u << 1)
#define PORT_PR   (1u << 4)
#define PORT_PP   (1u << 9)
#define PORT_CSC  (1u << 17)
#define PORT_PEC  (1u << 18)
#define PORT_WRC  (1u << 19)
#define PORT_OCC  (1u << 20)
#define PORT_RC   (1u << 21)
#define PORT_PLC  (1u << 22)
#define PORT_CEC  (1u << 23)
#define PORT_RW1C (PORT_CSC | PORT_PEC | PORT_WRC | PORT_OCC | PORT_RC | PORT_PLC | PORT_CEC)

/* TRB flags, types and completion codes. */
#define TRB_CYCLE       (1u << 0)
#define TRB_CHAIN       (1u << 4)
#define TRB_IOC         (1u << 5)
#define TRB_IDT         (1u << 6)
#define TRB_DIR_IN      (1u << 16)
#define TRB_TYPE(p)     ((u32)(p) << 10)
#define TRB_LINK_TOGGLE (1u << 1)

#define TRB_NORMAL             1u
#define TRB_SETUP              2u
#define TRB_DATA               3u
#define TRB_STATUS             4u
#define TRB_LINK               6u
#define TRB_NOOP               8u
#define TRB_ENABLE_SLOT        9u
#define TRB_ADDRESS_DEVICE     11u
#define TRB_CONFIGURE_ENDPOINT 12u
#define TRB_EVALUATE_CONTEXT   13u
#define TRB_EV_TRANSFER        32u
#define TRB_EV_COMMAND         33u
#define TRB_EV_PORT_CHANGE     34u

#define TRB_GET_TYPE(c) ((u32)((c) >> 10) & 0x3fu)
#define TRB_GET_COMP(s) ((u32)((s) >> 24) & 0xffu)
#define TRB_GET_SLOT(c) ((u32)((c) >> 24) & 0xffu)
#define TRB_GET_EP(c)   ((u32)((c) >> 16) & 0x1fu)

#define CTX_EP_CTRL      4u
#define CTX_EP_INTERRUPT 7u
#define COMP_SUCCESS     1u
#define COMP_SHORT       13u

#define XHCI_RING_TRBS  256u
#define XHCI_RING_LINK  (XHCI_RING_TRBS - 1u)
#define XHCI_EVENT_TRBS 256u
#define XHCI_MAX_PORTS  32u
#define XHCI_MAX_SLOTS  64u
#define XHCI_CTX_BYTES  4096u
#define XHCI_CTL_BYTES  512u
#define XHCI_SETUP_BYTES 8u

#define XHCI_CMD_TIMEOUT_NS  500000000ull
#define XHCI_PORT_TIMEOUT_NS 1000000000ull

#define USB_DESC_DEVICE 1u
#define USB_DESC_CONFIG 2u

typedef struct xhci_trb {
    u64 param;
    u32 status;
    u32 control;
} xhci_trb;

struct xhci_ring {
    xhci_trb *trbs;
    u64 pa;
    u32 enqueue;
    u32 cycle;
};

struct xhci_hc {
    volatile u8 *cap;
    volatile u8 *op;
    volatile u8 *rt;
    volatile u8 *db;
    u32 ctx_size;
    u32 ports;
    u32 max_slots;
    int present;
    struct xhci_ring cmd;
    struct xhci_ring ep0;
    struct xhci_ring hid;
    xhci_trb *ev;
    u64 ev_pa;
    u32 ev_dequeue;
    u32 ev_cycle;
    xhci_trb *erst;
    u64 *dcbaa;
    u64 dcbaa_pa;
    u8 *dev_ctx;
    u64 dev_ctx_pa;
    u8 *in_ctx;
    u64 in_ctx_pa;
    u32 slot;
    u32 speed;
    u32 port;
    u32 hid_dci;
    u32 kbd_port;
    int keyboard_ready;
    u32 keys;
    u32 wait_seconds;
    u32 want_keys;
};

static struct xhci_hc g_hc;
static u64 setup_param(const u8 *setup)
{
    u64 v = 0;
    u32 i;

    for (i = 0; i < XHCI_SETUP_BYTES; i++)
        v |= (u64)setup[i] << (8u * i);
    return v;
}
static u8 g_ctl[XHCI_CTL_BYTES] __attribute__((aligned(64)));
static u8 g_report[USB_HID_REPORT_BYTES] __attribute__((aligned(64)));

static u32 rd32(volatile u8 *base, u32 off)
{
    return *(volatile u32 *)(base + off);
}

static void wr32(volatile u8 *base, u32 off, u32 value)
{
    *(volatile u32 *)(base + off) = value;
}

static u64 rd64(volatile u8 *base, u32 off)
{
    return *(volatile u64 *)(base + off);
}

static void wr64(volatile u8 *base, u32 off, u64 value)
{
    *(volatile u64 *)(base + off) = value;
}

static void trb_set(xhci_trb *t, u64 param, u32 status, u32 control)
{
    t->param = param;
    t->status = status;
    t->control = control;
}

static u64 ring_trb_pa(const struct xhci_ring *r, u32 index)
{
    return r->pa + (u64)index * 16u;
}

static int ring_init(struct xhci_ring *r)
{
    u64 pa = pmm_alloc_pages_aligned(1, 1);

    if (!pa)
        return -1;
    r->trbs = (xhci_trb *)(uintptr)pa;
    r->pa = pa;
    r->enqueue = 0;
    r->cycle = 1;
    memset(r->trbs, 0, CRUG_PAGE_SIZE);
    trb_set(&r->trbs[XHCI_RING_LINK], r->pa, 0,
            TRB_TYPE(TRB_LINK) | TRB_LINK_TOGGLE | r->cycle);
    return 0;
}

/* Reserve room for n TRBs. A transfer descriptor never straddles the link. */
static int ring_reserve(struct xhci_ring *r, u32 n)
{
    u32 i;

    if (n == 0 || n > XHCI_RING_LINK)
        return -1;
    if (r->enqueue + n > XHCI_RING_LINK) {
        for (i = r->enqueue; i < XHCI_RING_LINK; i++)
            trb_set(&r->trbs[i], 0, 0, TRB_TYPE(TRB_NOOP) | r->cycle);
        trb_set(&r->trbs[XHCI_RING_LINK], r->pa, 0,
                TRB_TYPE(TRB_LINK) | TRB_LINK_TOGGLE | r->cycle);
        r->cycle ^= 1u;
        r->enqueue = 0;
    }
    return 0;
}

static xhci_trb *ring_next(struct xhci_ring *r, u32 *index)
{
    xhci_trb *t = &r->trbs[r->enqueue];

    if (index)
        *index = r->enqueue;
    r->enqueue++;
    return t;
}

static void xhci_doorbell(u32 target, u32 value)
{
    wr32(g_hc.db, target * 4u, value);
}

static void event_update_erdp(void)
{
    wr64(g_hc.rt, XHCI_RT_ERDP,
         (g_hc.ev_pa + (u64)g_hc.ev_dequeue * 16u) | XHCI_ERDP_EHB);
}

static int event_pop(xhci_trb *out)
{
    xhci_trb *t = &g_hc.ev[g_hc.ev_dequeue];

    if ((t->control & TRB_CYCLE) != g_hc.ev_cycle)
        return 0;
    if (out)
        *out = *t;
    g_hc.ev_dequeue++;
    if (g_hc.ev_dequeue == XHCI_EVENT_TRBS) {
        g_hc.ev_dequeue = 0;
        g_hc.ev_cycle ^= 1u;
    }
    event_update_erdp();
    return 1;
}

static u32 port_read(u32 port)
{
    return rd32(g_hc.op, XHCI_OP_PORTSC + (port - 1u) * XHCI_PORT_STRIDE);
}

static void port_write(u32 port, u32 value)
{
    wr32(g_hc.op, XHCI_OP_PORTSC + (port - 1u) * XHCI_PORT_STRIDE, value);
}

/* Clear the RW1C change bits without disturbing power and the enable state. */
static void port_clear_changes(u32 port)
{
    u32 v = port_read(port);

    port_write(port, (v & ~(PORT_PED | PORT_PR | PORT_RW1C)) | PORT_RW1C);
}

static void ctx_w32(u8 *ctx, u32 off, u32 value)
{
    *(u32 *)(ctx + off) = value;
}

static void ctx_w64(u8 *ctx, u32 off, u64 value)
{
    *(u64 *)(ctx + off) = value;
}

static u8 *in_slot_ctx(void)
{
    return g_hc.in_ctx + g_hc.ctx_size;
}

static u8 *in_ep_ctx(u32 dci)
{
    return g_hc.in_ctx + (dci + 1u) * g_hc.ctx_size;
}

static void input_ctx_reset(void)
{
    memset(g_hc.in_ctx, 0, XHCI_CTX_BYTES);
    ctx_w32(g_hc.in_ctx, 0x04, 0);
}

static void slot_ctx_fill(u8 *slot, u32 speed, u32 port, u32 entries)
{
    ctx_w32(slot, 0x00, (speed << 20) | (entries << 27));
    ctx_w32(slot, 0x04, port << 16);
    ctx_w32(slot, 0x08, 0);
    ctx_w32(slot, 0x0c, 0);
}

/* EP_TYPE in bits 3:5, CErr 3 in bits 1:2, max packet in bits 31:16. */
static void ep_ctx_fill(u8 *ep, u32 type, u32 max_packet, u32 interval, u64 deq, u32 avg)
{
    ctx_w32(ep, 0x00, interval << 16);
    ctx_w32(ep, 0x04, (type << 3) | (3u << 1) | (max_packet << 16));
    ctx_w64(ep, 0x08, deq | 1u);
    ctx_w32(ep, 0x10, avg);
    ctx_w32(ep, 0x14, 0);
}

static int xhci_command(u64 param, u32 type, u32 slot, u32 *out_comp, u32 *out_slot)
{
    struct xhci_ring *r = &g_hc.cmd;
    u32 index = 0;
    xhci_trb *t;
    u64 trb_pa;
    u64 deadline;

    if (ring_reserve(r, 1) != 0)
        return -1;
    t = ring_next(r, &index);
    trb_set(t, param, 0, TRB_TYPE(type) | (slot << 24) | r->cycle);
    trb_pa = ring_trb_pa(r, index);
    xhci_doorbell(0, 0);

    deadline = timer_monotonic_ns() + XHCI_CMD_TIMEOUT_NS;
    for (;;) {
        xhci_trb ev;

        while (event_pop(&ev)) {
            if (TRB_GET_TYPE(ev.control) == TRB_EV_COMMAND && ev.param == trb_pa) {
                if (out_comp)
                    *out_comp = TRB_GET_COMP(ev.status);
                if (out_slot)
                    *out_slot = TRB_GET_SLOT(ev.control);
                return 0;
            }
        }
        if (timer_monotonic_ns() > deadline) {
            kprintf("CRUGOS: usb command timed out (type %u)\n", (unsigned)type);
            return -1;
        }
        timer_busy_wait_us(50);
    }
}

static int xhci_port_reset(u32 port, u32 *speed)
{
    u64 deadline;
    u32 v;

    v = port_read(port);
    if (!(v & PORT_CCS))
        return -1;
    if (!(v & PORT_PP))
        port_write(port, v | PORT_PP);
    port_clear_changes(port);

    v = port_read(port);
    port_write(port, (v & ~(PORT_PED | PORT_PR | PORT_RW1C)) | PORT_PR);

    deadline = timer_monotonic_ns() + XHCI_PORT_TIMEOUT_NS;
    for (;;) {
        v = port_read(port);
        if (!(v & PORT_PR) && (v & PORT_RC))
            break;
        if (timer_monotonic_ns() > deadline) {
            kprintf("CRUGOS: usb port reset timeout\n");
            return -1;
        }
        timer_busy_wait_us(1000);
    }

    port_clear_changes(port);
    v = port_read(port);
    if (!(v & PORT_CCS) || !(v & PORT_PED)) {
        kprintf("CRUGOS: usb port not enabled\n");
        return -1;
    }
    if (speed)
        *speed = (v >> 10) & 0x0fu;
    return 0;
}

static int xhci_control(u32 slot, const u8 *setup, void *data, u32 len, int in)
{
    struct xhci_ring *r = &g_hc.ep0;
    u32 trbs = len ? 3u : 2u;
    u32 index = 0;
    xhci_trb *t;
    u64 deadline;
    u32 comp = 0;
    int done = 0;

    if (ring_reserve(r, trbs) != 0)
        return -1;

    /* With IDT the eight setup bytes travel inside the TRB parameter field. */
    t = ring_next(r, &index);
    trb_set(t, setup_param(setup), XHCI_SETUP_BYTES,
            TRB_TYPE(TRB_SETUP) | TRB_IDT | TRB_CHAIN | r->cycle);
    if (len) {
        t = ring_next(r, &index);
        trb_set(t, (u64)(uintptr)data, len,
                TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0) | TRB_CHAIN | r->cycle);
    }
    t = ring_next(r, &index);
    trb_set(t, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (in ? 0 : TRB_DIR_IN) | r->cycle);

    xhci_doorbell(slot, 1);

    deadline = timer_monotonic_ns() + XHCI_CMD_TIMEOUT_NS;
    for (;;) {
        xhci_trb ev;

        while (event_pop(&ev)) {
            if (TRB_GET_TYPE(ev.control) == TRB_EV_TRANSFER) {
                comp = TRB_GET_COMP(ev.status);
                done = 1;
                break;
            }
        }
        if (done)
            break;
        if (timer_monotonic_ns() > deadline)
            return -1;
        timer_busy_wait_us(50);
    }
    if (comp != COMP_SUCCESS && comp != COMP_SHORT) {
        kprintf("CRUGOS: usb control transfer failed (%u)\n", (unsigned)comp);
        return -1;
    }
    return 0;
}

static int xhci_get_descriptor(u32 slot, u8 type, void *buf, u32 len)
{
    u8 setup[XHCI_SETUP_BYTES];

    setup[0] = 0x80;
    setup[1] = 0x06;
    setup[2] = 0;
    setup[3] = type;
    setup[4] = 0;
    setup[5] = 0;
    setup[6] = (u8)(len & 0xffu);
    setup[7] = (u8)((len >> 8) & 0xffu);
    return xhci_control(slot, setup, buf, len, 1);
}

static int xhci_set_configuration(u32 slot)
{
    u8 setup[XHCI_SETUP_BYTES];

    setup[0] = 0x00;
    setup[1] = 0x09;
    setup[2] = 1;
    setup[3] = 0;
    setup[4] = 0;
    setup[5] = 0;
    setup[6] = 0;
    setup[7] = 0;
    return xhci_control(slot, setup, 0, 0, 0);
}

static int xhci_address_device(u32 slot, u32 speed, u32 port, u32 max_packet)
{
    u32 comp = 0;

    input_ctx_reset();
    ctx_w32(g_hc.in_ctx, 0x04, 0x3u);
    slot_ctx_fill(in_slot_ctx(), speed, port, 1);
    ep_ctx_fill(in_ep_ctx(1), CTX_EP_CTRL, max_packet, 0, g_hc.ep0.pa, 8);

    if (xhci_command(g_hc.in_ctx_pa, TRB_ADDRESS_DEVICE, slot, &comp, 0) != 0)
        return -1;
    if (comp != COMP_SUCCESS) {
        kprintf("CRUGOS: usb address device failed (%u)\n", (unsigned)comp);
        return -1;
    }
    return 0;
}

static int xhci_evaluate_context(u32 slot, u32 speed, u32 port, u32 max_packet)
{
    u32 comp = 0;

    input_ctx_reset();
    ctx_w32(g_hc.in_ctx, 0x04, 0x3u);
    slot_ctx_fill(in_slot_ctx(), speed, port, 1);
    ep_ctx_fill(in_ep_ctx(1), CTX_EP_CTRL, max_packet, 0, g_hc.ep0.pa, 8);

    if (xhci_command(g_hc.in_ctx_pa, TRB_EVALUATE_CONTEXT, slot, &comp, 0) != 0)
        return -1;
    if (comp != COMP_SUCCESS) {
        kprintf("CRUGOS: usb evaluate context failed (%u)\n", (unsigned)comp);
        return -1;
    }
    return 0;
}

static int xhci_configure_keyboard(u32 slot, const struct usb_hid_endpoint *ep)
{
    u32 dci = (u32)ep->ep_num * 2u + (ep->in ? 1u : 0u);
    u32 comp = 0;

    input_ctx_reset();
    ctx_w32(g_hc.in_ctx, 0x04, 0x1u | (1u << dci));
    slot_ctx_fill(in_slot_ctx(), g_hc.speed, g_hc.port, dci);
    ep_ctx_fill(in_ep_ctx(dci), CTX_EP_INTERRUPT, ep->max_packet, ep->interval,
                g_hc.hid.pa, ep->max_packet);

    if (xhci_command(g_hc.in_ctx_pa, TRB_CONFIGURE_ENDPOINT, slot, &comp, 0) != 0)
        return -1;
    if (comp != COMP_SUCCESS) {
        kprintf("CRUGOS: usb configure endpoint failed (%u)\n", (unsigned)comp);
        return -1;
    }
    g_hc.hid_dci = dci;
    return 0;
}

static int xhci_arm_keyboard(u32 slot, u32 dci)
{
    struct xhci_ring *r = &g_hc.hid;
    u32 index = 0;
    xhci_trb *t;

    if (ring_reserve(r, 1) != 0)
        return -1;
    t = ring_next(r, &index);
    trb_set(t, (u64)(uintptr)g_report, USB_HID_REPORT_BYTES,
            TRB_TYPE(TRB_NORMAL) | TRB_IOC | r->cycle);
    xhci_doorbell(slot, dci);
    return 0;
}

static int xhci_enumerate(u32 port, u32 speed)
{
    struct usb_hid_endpoint ep;
    u32 slot = 0;
    u32 comp = 0;
    u32 max_packet;
    u32 total;
    u32 read_len;

    if (!g_hc.dev_ctx) {
        u64 pa = pmm_alloc_pages_aligned(1, 1);

        if (!pa)
            return -1;
        g_hc.dev_ctx = (u8 *)(uintptr)pa;
        g_hc.dev_ctx_pa = pa;
    }
    memset(g_hc.dev_ctx, 0, CRUG_PAGE_SIZE);
    memset(g_hc.dcbaa, 0, CRUG_PAGE_SIZE);

    if (xhci_command(0, TRB_ENABLE_SLOT, 0, &comp, &slot) != 0) {
        kprintf("CRUGOS: usb enable slot timed out\n");
        return -1;
    }
    if (comp != COMP_SUCCESS || slot == 0 || slot > g_hc.max_slots) {
        kprintf("CRUGOS: usb enable slot failed (%u)\n", (unsigned)comp);
        return -1;
    }
    g_hc.dcbaa[slot] = g_hc.dev_ctx_pa;

    if (xhci_address_device(slot, speed, port, 8) != 0)
        return -1;

    if (xhci_get_descriptor(slot, USB_DESC_DEVICE, g_ctl, 8) != 0)
        return -1;
    max_packet = g_ctl[7];
    if (max_packet == 0)
        max_packet = 8;
    if (max_packet > 64)
        max_packet = 64;
    if (max_packet > 8 && xhci_evaluate_context(slot, speed, port, max_packet) != 0)
        return -1;

    if (xhci_get_descriptor(slot, USB_DESC_DEVICE, g_ctl, 18) != 0)
        return -1;
    if (g_ctl[0] < 18) {
        kprintf("CRUGOS: usb short device descriptor\n");
        return -1;
    }
    kprintf("CRUGOS: usb device %04x:%04x on port %u (speed %u)\n",
            (unsigned)(g_ctl[8] | ((u32)g_ctl[9] << 8)),
            (unsigned)(g_ctl[10] | ((u32)g_ctl[11] << 8)),
            (unsigned)port, (unsigned)speed);

    if (xhci_get_descriptor(slot, USB_DESC_CONFIG, g_ctl, 9) != 0)
        return -1;
    total = (u32)g_ctl[2] | ((u32)g_ctl[3] << 8);
    if (total < 9) {
        kprintf("CRUGOS: usb bad configuration length\n");
        return -1;
    }
    read_len = total > XHCI_CTL_BYTES ? XHCI_CTL_BYTES : total;
    if (xhci_get_descriptor(slot, USB_DESC_CONFIG, g_ctl, read_len) != 0)
        return -1;

    if (usb_hid_parse_config(g_ctl, read_len, &ep) != 0) {
        kprintf("CRUGOS: usb device is not a boot keyboard\n");
        return -1;
    }
    if (ep.in == 0 || ep.ep_num == 0 || ep.max_packet == 0) {
        kprintf("CRUGOS: usb keyboard endpoint is invalid\n");
        return -1;
    }

    if (xhci_set_configuration(slot) != 0)
        return -1;

    g_hc.slot = slot;
    g_hc.speed = speed;
    g_hc.port = port;
    if (xhci_configure_keyboard(slot, &ep) != 0)
        return -1;
    if (xhci_arm_keyboard(slot, g_hc.hid_dci) != 0)
        return -1;

    g_hc.keyboard_ready = 1;
    g_hc.kbd_port = port;
    g_hc.keys = 0;
    usb_hid_reset();
    kprintf("CRUGOS: usb keyboard ready (endpoint 0x%02x, %u byte reports)\n",
            (unsigned)ep.ep_addr, (unsigned)USB_HID_REPORT_BYTES);
    return 0;
}

static void xhci_scan_ports(void)
{
    u32 port;

    for (port = 1; port <= g_hc.ports && port <= XHCI_MAX_PORTS; port++) {
        u32 v = port_read(port);
        int connected = (v & PORT_CCS) ? 1 : 0;

        if (g_hc.keyboard_ready && port == g_hc.kbd_port && !connected) {
            g_hc.keyboard_ready = 0;
            g_hc.slot = 0;
            g_hc.hid_dci = 0;
            usb_hid_reset();
            kprintf("CRUGOS: usb keyboard gone\n");
        }
        if (v & PORT_RW1C)
            port_clear_changes(port);
        if (!connected || g_hc.keyboard_ready)
            continue;
        {
            u32 speed = 0;

            if (xhci_port_reset(port, &speed) != 0)
                continue;
            if (xhci_enumerate(port, speed) == 0)
                break;
        }
    }
}

static void report_marker(int ch)
{
    if (ch == 0x0a)
        kprintf("CRUGOS: usb key enter\n");
    else if (ch == 0x09)
        kprintf("CRUGOS: usb key tab\n");
    else if (ch == 0x7f)
        kprintf("CRUGOS: usb key backspace\n");
    else if (ch == 0x1b)
        kprintf("CRUGOS: usb key esc\n");
    else if (ch >= 0x20 && ch < 0x7f)
        kprintf("CRUGOS: usb key '%c'\n", (char)ch);
    else
        kprintf("CRUGOS: usb key 0x%02x\n", (unsigned)ch);
}

void usb_poll(void)
{
    xhci_trb ev;
    u32 guard;

    if (!g_hc.present)
        return;

    for (guard = 0; guard < XHCI_EVENT_TRBS; guard++) {
        u32 type;

        if (!event_pop(&ev))
            break;
        type = TRB_GET_TYPE(ev.control);
        if (type == TRB_EV_PORT_CHANGE) {
            xhci_scan_ports();
        } else if (type == TRB_EV_TRANSFER) {
            u32 slot = TRB_GET_SLOT(ev.control);
            u32 dci = TRB_GET_EP(ev.control);
            u32 comp = TRB_GET_COMP(ev.status);

            if (!g_hc.keyboard_ready || slot != g_hc.slot || dci != g_hc.hid_dci)
                continue;
            if (comp == COMP_SUCCESS || comp == COMP_SHORT) {
                int ch = usb_hid_boot_decode(g_report);

                if (ch > 0) {
                    g_hc.keys++;
                    report_marker(ch);
                    input_char((u8)ch);
                }
            } else {
                kprintf("CRUGOS: usb transfer failed (%u)\n", (unsigned)comp);
            }
            (void)xhci_arm_keyboard(g_hc.slot, g_hc.hid_dci);
        }
    }
}

int usb_present(void)
{
    return g_hc.present;
}

int usb_keyboard_ready(void)
{
    return g_hc.keyboard_ready;
}

static int usb_prefix(const char *tok, usize len, const char *prefix)
{
    usize n = strlen(prefix);

    if (n > len)
        return 0;
    return memcmp(tok, prefix, n) == 0;
}

static int usb_parse_u32(const char *p, usize len, u32 *out)
{
    usize i;
    u64 v = 0;

    if (len == 0)
        return -1;
    for (i = 0; i < len; i++) {
        if (p[i] < '0' || p[i] > '9')
            return -1;
        v = v * 10u + (u64)(p[i] - '0');
        if (v > 4096u)
            v = 4096u;
    }
    *out = (u32)v;
    return 0;
}

static void usb_cmdline_token(const char *tok, usize len)
{
    u32 value;

    if (usb_prefix(tok, len, "usb.wait=")) {
        if (usb_parse_u32(tok + 9, len - 9, &value) == 0)
            g_hc.wait_seconds = value > 120u ? 120u : value;
    } else if (usb_prefix(tok, len, "usb.keys=")) {
        if (usb_parse_u32(tok + 9, len - 9, &value) == 0)
            g_hc.want_keys = value > 16u ? 16u : value;
    }
}

void usb_cmdline(const boot_info *bi)
{
    const char *s;
    usize len;
    usize i = 0;

    if (!bi || !bi->command_line || bi->command_line_len == 0)
        return;
    s = bi->command_line;
    len = (usize)bi->command_line_len;

    while (i < len) {
        usize start;

        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n'))
            i++;
        start = i;
        while (i < len && !(s[i] == ' ' || s[i] == '\t' || s[i] == '\n'))
            i++;
        if (i > start)
            usb_cmdline_token(s + start, i - start);
    }
}

/* Size a memory BAR by writing all ones and reading the mask back. */
static u64 pci_bar_size(u8 bus, u8 dev, u8 fn, u32 index, int is64)
{
    u32 off = 0x10u + index * 4u;
    u32 orig = pci_read32(bus, dev, fn, off);
    u32 orig_hi = is64 ? pci_read32(bus, dev, fn, off + 4u) : 0;
    u64 mask;
    u64 size;

    pci_write32(bus, dev, fn, off, 0xffffffffu);
    if (is64)
        pci_write32(bus, dev, fn, off + 4u, 0xffffffffu);
    mask = (u64)(pci_read32(bus, dev, fn, off) & 0xfffff000u);
    if (is64)
        mask |= ((u64)(pci_read32(bus, dev, fn, off + 4u) & 0xfffff000u)) << 32;
    else
        mask |= 0xffffffff00000000ull;
    pci_write32(bus, dev, fn, off, orig);
    if (is64)
        pci_write32(bus, dev, fn, off + 4u, orig_hi);

    size = ~mask + 1ull;
    if (size == 0 || size > (64ull << 20))
        return 0;
    return size;
}

/* Is [base, base+size) clear of every other root-bus BAR? */
static int pci_win_free(u64 base, u64 size, u8 skip_dev, u8 skip_fn)
{
    u32 dev;
    u32 fn;

    for (dev = 0; dev < 32u; dev++) {
        for (fn = 0; fn < 8u; fn++) {
            u32 vendor;
            u32 i;

            if ((u8)dev == skip_dev && (u8)fn == skip_fn)
                continue;
            vendor = pci_read32(0, (u8)dev, (u8)fn, 0x00);
            if ((vendor & 0xffffu) == 0xffffu) {
                if (fn == 0u)
                    break;
                continue;
            }
            for (i = 0; i < 6u; i++) {
                u32 low = pci_read32(0, (u8)dev, (u8)fn, 0x10u + i * 4u);
                u64 other;
                u64 osize;
                int is64;

                if (low & 1u)
                    continue;
                is64 = ((low >> 1) & 0x3u) == 0x2u;
                other = (u64)(low & 0xfffff000u);
                if (is64)
                    other |= ((u64)pci_read32(0, (u8)dev, (u8)fn, 0x14u + i * 4u)) << 32;
                if (other == 0) {
                    if (is64)
                        i++;
                    continue;
                }
                osize = pci_bar_size(0, (u8)dev, (u8)fn, i, is64);
                if (osize != 0 && other < base + size && base < other + osize)
                    return 0;
                if (is64)
                    i++;
            }
        }
    }
    return 1;
}

/* OVMF ships no xHCI driver and may leave the BAR unassigned. Look for a free
   window in the 32-bit PCI hole, just below the firmware flash at 0xffc00000. */
static int pci_pick_mmio(u64 size, u8 dev, u8 fn, u64 *out)
{
    u32 step;

    for (step = 0; step < 512u; step++) {
        u64 cand = 0xffb00000ull - (u64)step * 0x100000ull;

        if (cand + size > 0xffc00000ull)
            continue;
        if (cand < 0x80000000ull)
            break;
        if (pci_win_free(cand, size, dev, fn)) {
            *out = cand;
            return 0;
        }
    }
    return -1;
}

int usb_init(void)
{
    u8 bus = 0;
    u8 dev = 0;
    u8 fn = 0;
    u64 mask;
    u64 size;
    u64 pa;
    u32 bar;
    u32 bar_hi = 0;
    u32 orig_hi = 0;
    int is64;
    u32 hcsp1;
    u32 hcc1;
    u32 off;
    u64 deadline;

    memset(&g_hc, 0, sizeof(g_hc));
    g_hc.wait_seconds = 0;
    g_hc.want_keys = 0;
    usb_hid_reset();
    input_set_poll(usb_poll);

    if (pci_find_class_nth(0x0cu, 0x03u, 0x30u, 0, &bus, &dev, &fn) != 1) {
        kprintf("CRUGOS: usb controller absent\n");
        return -1;
    }
    pci_enable(bus, dev, fn, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    bar = pci_bar(bus, dev, fn, 0);
    if (bar & PCI_BAR_IO) {
        kprintf("CRUGOS: usb controller absent (I/O BAR)\n");
        return -1;
    }
    is64 = ((bar >> 1) & 0x3u) == 0x2u;
    if (is64) {
        orig_hi = pci_bar(bus, dev, fn, 1);
        bar_hi = orig_hi & 0xfffff000u;
    }
    pa = ((u64)bar_hi << 32) | (u64)(bar & 0xfffff000u);

    pci_write32(bus, dev, fn, 0x10, 0xffffffffu);
    if (is64)
        pci_write32(bus, dev, fn, 0x14, 0xffffffffu);
    mask = (u64)(pci_read32(bus, dev, fn, 0x10) & 0xfffff000u);
    if (is64)
        mask |= ((u64)(pci_read32(bus, dev, fn, 0x14) & 0xfffff000u)) << 32;
    else
        mask |= 0xffffffff00000000ull;
    pci_write32(bus, dev, fn, 0x10, bar);
    if (is64)
        pci_write32(bus, dev, fn, 0x14, orig_hi);

    size = ~mask + 1ull;
    if (size < CRUG_PAGE_SIZE)
        size = CRUG_PAGE_SIZE;
    if (size > 65536ull)
        size = 65536ull;

    if (pa == 0) {
        u64 chosen = 0;

        if (pci_pick_mmio(size, dev, fn, &chosen) != 0) {
            kprintf("CRUGOS: usb controller absent (no MMIO window)\n");
            return -1;
        }
        pci_write32(bus, dev, fn, 0x10, (u32)(chosen & 0xfffff000u) | (bar & 0x0fu));
        if (is64)
            pci_write32(bus, dev, fn, 0x14, (u32)(chosen >> 32));
        pa = chosen;
        kprintf("CRUGOS: usb controller BAR assigned at 0x%08x\n", (unsigned)pa);
    }

    for (off = 0; off < (u32)size; off += CRUG_PAGE_SIZE) {
        u64 va = pa + (u64)off;
        u64 flags = VM_PRESENT | VM_WRITE | VM_NX | VM_PCD | VM_PWT | VM_GLOBAL;

        if (vm_translate(va) == va) {
            if (vm_protect_page(va, flags) != 0) {
                kprintf("CRUGOS: usb controller absent (map)\n");
                return -1;
            }
        } else if (vm_map_page(va, va, flags) != 0) {
            kprintf("CRUGOS: usb controller absent (map)\n");
            return -1;
        }
    }

    g_hc.cap = (volatile u8 *)(uintptr)pa;
    g_hc.op = g_hc.cap + (rd32(g_hc.cap, XHCI_CAP_CAPLENGTH) & 0xffu);
    g_hc.rt = g_hc.cap + rd32(g_hc.cap, XHCI_CAP_RTSOFF);
    g_hc.db = g_hc.cap + rd32(g_hc.cap, XHCI_CAP_DBOFF);

    /* Stop the controller, then reset it: OVMF may have used it already. */
    wr32(g_hc.op, XHCI_OP_USBCMD, rd32(g_hc.op, XHCI_OP_USBCMD) & ~XHCI_CMD_RUN);
    deadline = timer_monotonic_ns() + XHCI_CMD_TIMEOUT_NS;
    while (!(rd32(g_hc.op, XHCI_OP_USBSTS) & XHCI_STS_HALT)) {
        if (timer_monotonic_ns() > deadline)
            break;
        timer_busy_wait_us(1000);
    }
    wr32(g_hc.op, XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    deadline = timer_monotonic_ns() + XHCI_CMD_TIMEOUT_NS;
    for (;;) {
        u32 cmd = rd32(g_hc.op, XHCI_OP_USBCMD);
        u32 sts = rd32(g_hc.op, XHCI_OP_USBSTS);

        if (!(cmd & XHCI_CMD_HCRST) && !(sts & XHCI_STS_CNR))
            break;
        if (timer_monotonic_ns() > deadline) {
            kprintf("CRUGOS: usb controller absent (reset timeout)\n");
            return -1;
        }
        timer_busy_wait_us(1000);
    }

    hcsp1 = rd32(g_hc.cap, XHCI_CAP_HCSPARAMS1);
    hcc1 = rd32(g_hc.cap, XHCI_CAP_HCCPARAMS1);
    g_hc.ctx_size = (hcc1 & 0x4u) ? 64u : 32u;
    g_hc.max_slots = hcsp1 & 0xffu;
    g_hc.ports = (hcsp1 >> 24) & 0xffu;
    if (g_hc.max_slots == 0 || g_hc.max_slots > XHCI_MAX_SLOTS)
        g_hc.max_slots = XHCI_MAX_SLOTS;
    if (g_hc.ports == 0)
        g_hc.ports = 1;
    if (g_hc.ports > XHCI_MAX_PORTS)
        g_hc.ports = XHCI_MAX_PORTS;

    if (ring_init(&g_hc.cmd) != 0 || ring_init(&g_hc.ep0) != 0 ||
        ring_init(&g_hc.hid) != 0) {
        kprintf("CRUGOS: usb controller absent (rings)\n");
        return -1;
    }

    {
        u64 ev_pa = pmm_alloc_pages_aligned(1, 1);
        u64 erst_pa = pmm_alloc_pages_aligned(1, 1);
        u64 dcbaa_pa = pmm_alloc_pages_aligned(1, 1);
        u64 in_pa = pmm_alloc_pages_aligned(1, 1);

        if (!ev_pa || !erst_pa || !dcbaa_pa || !in_pa) {
            kprintf("CRUGOS: usb controller absent (memory)\n");
            return -1;
        }
        g_hc.ev = (xhci_trb *)(uintptr)ev_pa;
        g_hc.ev_pa = ev_pa;
        g_hc.ev_dequeue = 0;
        g_hc.ev_cycle = 1;
        memset(g_hc.ev, 0, CRUG_PAGE_SIZE);

        g_hc.erst = (xhci_trb *)(uintptr)erst_pa;
        memset(g_hc.erst, 0, CRUG_PAGE_SIZE);
        g_hc.erst[0].param = ev_pa;
        g_hc.erst[0].status = XHCI_EVENT_TRBS;
        g_hc.erst[0].control = 0;

        g_hc.dcbaa = (u64 *)(uintptr)dcbaa_pa;
        g_hc.dcbaa_pa = dcbaa_pa;
        memset(g_hc.dcbaa, 0, CRUG_PAGE_SIZE);

        g_hc.in_ctx = (u8 *)(uintptr)in_pa;
        g_hc.in_ctx_pa = in_pa;
        memset(g_hc.in_ctx, 0, CRUG_PAGE_SIZE);
    }

    wr32(g_hc.op, XHCI_OP_CONFIG, g_hc.max_slots);
    wr64(g_hc.op, XHCI_OP_DCBAAP, g_hc.dcbaa_pa);
    wr64(g_hc.op, XHCI_OP_CRCR, g_hc.cmd.pa | 1u);
    if (rd64(g_hc.op, XHCI_OP_CRCR) != (g_hc.cmd.pa | 1u)) {
        kprintf("CRUGOS: usb controller absent (command ring)\n");
        return -1;
    }

    wr32(g_hc.rt, XHCI_RT_ERSTSZ, 1);
    wr64(g_hc.rt, XHCI_RT_ERSTBA, (u64)(uintptr)g_hc.erst);
    wr64(g_hc.rt, XHCI_RT_ERDP, g_hc.ev_pa | XHCI_ERDP_EHB);

    wr32(g_hc.op, XHCI_OP_USBCMD, rd32(g_hc.op, XHCI_OP_USBCMD) | XHCI_CMD_RUN);
    deadline = timer_monotonic_ns() + XHCI_CMD_TIMEOUT_NS;
    while (rd32(g_hc.op, XHCI_OP_USBSTS) & XHCI_STS_HALT) {
        if (timer_monotonic_ns() > deadline) {
            kprintf("CRUGOS: usb controller absent (halted)\n");
            return -1;
        }
        timer_busy_wait_us(1000);
    }

    g_hc.present = 1;
    kprintf("CRUGOS: usb controller ready (xhci, %u port(s))\n", (unsigned)g_hc.ports);

    xhci_scan_ports();
    return 0;
}

int usb_selftest(void)
{
    u64 deadline;
    u32 keys;

    if (!g_hc.present)
        return 1;
    if (g_hc.wait_seconds == 0)
        return 1;

    deadline = timer_monotonic_ns() + (u64)g_hc.wait_seconds * 1000000000ull;
    while (!g_hc.keyboard_ready) {
        usb_poll();
        if (g_hc.keyboard_ready)
            break;
        if (timer_monotonic_ns() >= deadline) {
            kprintf("CRUGOS: usb keyboard timeout\n");
            return 0;
        }
        cpu_wait_for_irq();
    }

    keys = g_hc.keys;
    if (g_hc.want_keys) {
        deadline = timer_monotonic_ns() + (u64)g_hc.wait_seconds * 1000000000ull;
        for (;;) {
            usb_poll();
            keys = g_hc.keys;
            if (keys >= g_hc.want_keys)
                break;
            if (timer_monotonic_ns() >= deadline) {
                kprintf("CRUGOS: usb keyboard timeout (keys %u/%u)\n",
                        (unsigned)keys, (unsigned)g_hc.want_keys);
                return 0;
            }
            cpu_wait_for_irq();
        }
    }

    kprintf("CRUGOS: usb selftest ok (%u key(s))\n", (unsigned)keys);
    return 1;
}
