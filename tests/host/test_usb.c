/* Host tests for the USB HID boot keyboard helpers (drivers/usb/usb_hid.c). */
#include <crug/types.h>
#include <crug/usb.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

/* Build a configuration descriptor in a fixed buffer, patch its total length. */
static u8 g_cfg[128];
static u32 g_cfg_len;

static void cfg_begin(void)
{
    memset(g_cfg, 0, sizeof(g_cfg));
    g_cfg[0] = 9;
    g_cfg[1] = 0x02;   /* configuration */
    g_cfg[4] = 1;      /* bNumInterfaces */
    g_cfg[5] = 1;      /* iConfiguration */
    g_cfg[7] = 0x80;   /* bus powered */
    g_cfg[8] = 50;     /* 100 mA */
    g_cfg_len = 9;
}

static void cfg_interface(u8 num, u8 cls, u8 sub, u8 proto)
{
    u8 *d = &g_cfg[g_cfg_len];

    d[0] = 9;
    d[1] = 0x04;
    d[2] = num;
    d[3] = 0;
    d[4] = 1;      /* one endpoint */
    d[5] = cls;
    d[6] = sub;
    d[7] = proto;
    d[8] = 0;
    g_cfg_len += 9;
}

static void cfg_hid(void)
{
    u8 *d = &g_cfg[g_cfg_len];

    d[0] = 9;
    d[1] = 0x21;
    d[2] = 0x11;
    d[3] = 0x01;
    d[4] = 0;
    d[5] = 1;
    d[6] = 0x22;
    d[7] = 30;
    d[8] = 0;
    g_cfg_len += 9;
}

static void cfg_endpoint(u8 addr, u8 attr, u16 packet, u8 interval)
{
    u8 *d = &g_cfg[g_cfg_len];

    d[0] = 7;
    d[1] = 0x05;
    d[2] = addr;
    d[3] = attr;
    d[4] = (u8)(packet & 0xffu);
    d[5] = (u8)(packet >> 8);
    d[6] = interval;
    g_cfg_len += 7;
}

static void cfg_finish(void)
{
    g_cfg[2] = (u8)(g_cfg_len & 0xffu);
    g_cfg[3] = (u8)(g_cfg_len >> 8);
}

/* A well formed boot keyboard: interface 0, HID descriptor, EP1 IN interrupt. */
static void build_boot_keyboard(void)
{
    cfg_begin();
    cfg_interface(0, 3, 1, 1);
    cfg_hid();
    cfg_endpoint(0x81, 0x03, 8, 10);
    cfg_finish();
}

static int parse(struct usb_hid_endpoint *ep)
{
    return usb_hid_parse_config(g_cfg, g_cfg_len, ep);
}

static int decode_report(u8 mods, u8 k0, u8 k1)
{
    u8 report[USB_HID_REPORT_BYTES];

    memset(report, 0, sizeof(report));
    report[0] = mods;
    report[1] = k0;
    report[2] = k1;
    return usb_hid_boot_decode(report);
}

void test_usb_functions(void)
{
    struct usb_hid_endpoint ep;
    u8 report[USB_HID_REPORT_BYTES];

    /* ---- descriptor parsing ---- */
    build_boot_keyboard();
    CHECK(parse(&ep) == 0);
    CHECK(ep.ep_addr == 0x81);
    CHECK(ep.ep_num == 1);
    CHECK(ep.in == 1);
    CHECK(ep.max_packet == 8);
    CHECK(ep.interval == 10);

    /* A boot mouse: same interface class, protocol 2, 4 byte reports. */
    cfg_begin();
    cfg_interface(0, 3, 1, 2);
    cfg_hid();
    cfg_endpoint(0x81, 0x03, 4, 10);
    cfg_finish();
    CHECK(parse(&ep) == 0);
    CHECK(ep.kind == USB_HID_KIND_MOUSE);
    CHECK(ep.ep_addr == 0x81);
    CHECK(ep.max_packet == 4);

    /* A full speed endpoint with a 64 byte packet still reports it. */
    cfg_begin();
    cfg_interface(0, 3, 1, 1);
    cfg_hid();
    cfg_endpoint(0x82, 0x03, 64, 5);
    cfg_finish();
    CHECK(parse(&ep) == 0);
    CHECK(ep.ep_addr == 0x82);
    CHECK(ep.ep_num == 2);
    CHECK(ep.max_packet == 64);
    CHECK(ep.interval == 5);

    /* High bits of wMaxPacketSize (transaction opportunities) are masked. */
    cfg_begin();
    cfg_interface(0, 3, 1, 1);
    cfg_endpoint(0x83, 0x03, 0x0c00, 4);
    cfg_finish();
    CHECK(parse(&ep) == 0);
    CHECK(ep.max_packet == 0x400);

    /* A non boot protocol keyboard interface is not a boot keyboard. */
    cfg_begin();
    cfg_interface(0, 3, 1, 0);
    cfg_endpoint(0x81, 0x03, 8, 10);
    cfg_finish();
    CHECK(parse(&ep) == -1);

    /* A HID interface without an interrupt IN endpoint is not usable. */
    cfg_begin();
    cfg_interface(0, 3, 1, 1);
    cfg_hid();
    cfg_finish();
    CHECK(parse(&ep) == -1);

    /* An OUT endpoint is not the report endpoint we need. */
    cfg_begin();
    cfg_interface(0, 3, 1, 1);
    cfg_endpoint(0x01, 0x03, 8, 10);
    cfg_finish();
    CHECK(parse(&ep) == -1);

    /* A bulk endpoint is not an interrupt endpoint. */
    cfg_begin();
    cfg_interface(0, 3, 1, 1);
    cfg_endpoint(0x81, 0x02, 8, 10);
    cfg_finish();
    CHECK(parse(&ep) == -1);

    /* A later interface resets the boot keyboard state. */
    cfg_begin();
    cfg_interface(0, 3, 1, 1);
    cfg_interface(1, 0xff, 0, 0);
    cfg_endpoint(0x81, 0x03, 8, 10);
    cfg_finish();
    CHECK(parse(&ep) == -1);

    /* A zero bLength must be rejected instead of looping forever. */
    cfg_begin();
    g_cfg[9] = 0;
    g_cfg[10] = 0x04;
    g_cfg_len = 11;
    CHECK(parse(&ep) == -1);

    /* A descriptor that runs past the buffer must be rejected. */
    cfg_begin();
    g_cfg[9] = 40;
    g_cfg[10] = 0x04;
    g_cfg_len = 12;
    CHECK(parse(&ep) == -1);

    /* Degenerate arguments. */
    CHECK(usb_hid_parse_config(NULL, 34, &ep) == -1);
    CHECK(usb_hid_parse_config(g_cfg, 8, &ep) == -1);
    build_boot_keyboard();
    CHECK(usb_hid_parse_config(g_cfg, g_cfg_len, NULL) == -1);

    /* ---- report decoding ---- */
    usb_hid_reset();
    CHECK(decode_report(0, 0x04, 0) == 'a');
    CHECK(decode_report(0, 0x04, 0) == 0);          /* held key repeats */
    CHECK(decode_report(0, 0, 0) == 0);             /* release */
    CHECK(decode_report(0, 0x04, 0) == 'a');       /* pressed again */
    CHECK(decode_report(0, 0x1d, 0) == 'z');

    usb_hid_reset();
    CHECK(decode_report(0x02, 0x04, 0) == 'A');    /* left shift */
    CHECK(decode_report(0x20, 0x05, 0) == 'B');    /* right shift */
    CHECK(decode_report(0x01, 0x06, 0) == 3);       /* ctrl+c */
    CHECK(decode_report(0x03, 0x07, 0) == 4);       /* ctrl+shift+d */
    CHECK(decode_report(0x10, 0x08, 0) == 5);       /* right ctrl+e */
    CHECK(decode_report(0x01, 0x2f, 0) == 27);      /* ctrl+[ */
    CHECK(decode_report(0x01, 0x2c, 0) == 0);       /* ctrl+space is dropped */
    CHECK(decode_report(0x03, 0x1f, 0) == 0);       /* ctrl+shift+2 = ctrl+@ */

    usb_hid_reset();
    CHECK(decode_report(0, 0x1e, 0) == '1');
    CHECK(decode_report(0, 0x27, 0) == '0');
    CHECK(decode_report(0x02, 0x1e, 0) == '!');
    CHECK(decode_report(0x02, 0x27, 0) == ')');

    usb_hid_reset();
    CHECK(decode_report(0, 0x2d, 0) == '-');
    CHECK(decode_report(0, 0x2e, 0) == '=');
    CHECK(decode_report(0, 0x2f, 0) == '[');
    CHECK(decode_report(0, 0x33, 0) == ';');
    CHECK(decode_report(0, 0x35, 0) == '`');
    CHECK(decode_report(0, 0x38, 0) == '/');
    CHECK(decode_report(0, 0x32, 0) == 0);          /* unused usage */
    CHECK(decode_report(0x02, 0x2d, 0) == '_');
    CHECK(decode_report(0x02, 0x38, 0) == '?');
    CHECK(decode_report(0x02, 0x35, 0) == '~');

    usb_hid_reset();
    CHECK(decode_report(0, 0x28, 0) == 0x0a);       /* enter */
    CHECK(decode_report(0, 0x29, 0) == 0x1b);       /* escape */
    CHECK(decode_report(0, 0x2a, 0) == 0x7f);       /* backspace */
    CHECK(decode_report(0, 0x2b, 0) == 0x09);       /* tab */
    CHECK(decode_report(0, 0x2c, 0) == ' ');       /* space */
    CHECK(decode_report(0, 0x39, 0) == 0);          /* caps is a latch */
    CHECK(decode_report(0, 0x39, 0) == 0);          /* no double toggle */
    CHECK(decode_report(0, 0, 0) == 0);
    CHECK(decode_report(0, 0x04, 0) == 'A');       /* caps let go */
    CHECK(decode_report(0, 0x39, 0) == 0);
    CHECK(decode_report(0, 0, 0) == 0);
    CHECK(decode_report(0, 0x04, 0) == 'a');       /* caps off again */
    CHECK(decode_report(0x02, 0x04, 0) == 0);      /* still held */
    CHECK(decode_report(0, 0, 0) == 0);
    CHECK(decode_report(0x02, 0x04, 0) == 'A');    /* shift, caps off */

    /* Modifier usages in the key array are ignored, and a report with two new
       keys reports the first one only. */
    usb_hid_reset();
    CHECK(decode_report(0, 0xe1, 0) == 0);
    CHECK(decode_report(0, 0x04, 0x05) == 'a');
    CHECK(decode_report(0, 0x05, 0) == 0);
    CHECK(decode_report(0, 0x05, 0x06) == 'c');

    /* Reset clears the latched caps state and the pressed key history. */
    usb_hid_reset();
    memset(report, 0, sizeof(report));
    CHECK(usb_hid_boot_decode(NULL) == 0);
    CHECK(decode_report(0, 0x39, 0) == 0);
    CHECK(decode_report(0, 0, 0) == 0);
    CHECK(decode_report(0, 0x04, 0) == 'A');
    usb_hid_reset();
    CHECK(decode_report(0, 0x04, 0) == 'a');
    CHECK(decode_report(0, 0x04, 0) == 0);

    /* ---- boot mouse decoding ---- */
    {
        struct usb_hid_pointer p;
        u8 m[USB_HID_MOUSE_REPORT_BYTES];

        usb_hid_reset();
        memset(m, 0, sizeof(m));

        /* Idle report: no motion, no buttons, no event. */
        CHECK(usb_hid_mouse_decode(m, 4, &p) == 0);
        CHECK(p.dx == 0 && p.dy == 0 && p.buttons == 0 && p.changes == 0);

        /* Positive and negative motion are sign extended from one byte. */
        m[1] = 5;
        m[2] = 0x03;
        CHECK(usb_hid_mouse_decode(m, 4, &p) == 1);
        CHECK(p.dx == 5 && p.dy == 3);
        CHECK(p.buttons == 0 && p.changes == 0);

        m[1] = 0xfb;   /* -5 */
        m[2] = 0xf0;   /* -16 */
        CHECK(usb_hid_mouse_decode(m, 4, &p) == 1);
        CHECK(p.dx == -5 && p.dy == -16);

        /* Buttons report changes only when the state actually changes. */
        memset(m, 0, sizeof(m));
        m[0] = 0x01;
        CHECK(usb_hid_mouse_decode(m, 4, &p) == 1);
        CHECK(p.buttons == 1 && p.changes == 1);
        CHECK(usb_hid_mouse_decode(m, 4, &p) == 0);
        m[0] = 0x03;
        CHECK(usb_hid_mouse_decode(m, 4, &p) == 1);
        CHECK(p.changes == 0x02);
        m[0] = 0;
        CHECK(usb_hid_mouse_decode(m, 4, &p) == 1);
        CHECK(p.changes == 0x03);

        /* The wheel byte is signed and counts as an event on its own. */
        m[3] = 0xff;
        CHECK(usb_hid_mouse_decode(m, 4, &p) == 1);
        CHECK(p.wheel == -1);
        m[3] = 0;

        /* A 3 byte report has no wheel byte. */
        m[1] = 1;
        CHECK(usb_hid_mouse_decode(m, 3, &p) == 1);
        CHECK(p.dx == 1 && p.wheel == 0);
        m[1] = 0;

        /* Reserved button bits are masked off. */
        m[0] = 0xf8;
        CHECK(usb_hid_mouse_decode(m, 4, &p) == 0);
        CHECK(p.buttons == 0);

        /* Degenerate arguments. */
        CHECK(usb_hid_mouse_decode(NULL, 4, &p) == -1);
        CHECK(usb_hid_mouse_decode(m, 2, &p) == -1);
        CHECK(usb_hid_mouse_decode(m, 4, NULL) == -1);

        /* Reset forgets the button state: the next report is a fresh baseline
           and the one after that reports the change. */
        usb_hid_reset();
        m[0] = 0x02;
        CHECK(usb_hid_mouse_decode(m, 4, &p) == 0);
        CHECK(p.buttons == 0x02 && p.changes == 0);
        m[0] = 0x03;
        CHECK(usb_hid_mouse_decode(m, 4, &p) == 1);
        CHECK(p.changes == 0x01);
    }
}
