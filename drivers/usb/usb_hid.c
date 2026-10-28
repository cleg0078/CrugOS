/* USB HID boot keyboard and boot mouse: configuration parsing and report
   decoding. */
#include <crug/types.h>
#include <crug/usb.h>
#include <crug/input.h>
#include <crug/string.h>

#define HID_USAGE_SLOTS 6u

#define HID_MOD_LCTRL  0x01u
#define HID_MOD_LSHIFT 0x02u
#define HID_MOD_LALT   0x04u
#define HID_MOD_RCTRL  0x10u
#define HID_MOD_RSHIFT 0x20u
#define HID_MOD_RALT   0x40u

#define HID_CTRL_MASK  (HID_MOD_LCTRL | HID_MOD_RCTRL)
#define HID_SHIFT_MASK (HID_MOD_LSHIFT | HID_MOD_RSHIFT)

#define HID_USAGE_MIN         0x04u
#define HID_USAGE_LETTER_END  0x1du
#define HID_USAGE_DIGIT_MIN   0x1eu
#define HID_USAGE_DIGIT_MAX   0x27u
#define HID_USAGE_ENTER       0x28u
#define HID_USAGE_ESC         0x29u
#define HID_USAGE_BACKSPACE   0x2au
#define HID_USAGE_TAB         0x2bu
#define HID_USAGE_SPACE       0x2cu
#define HID_USAGE_PUNCT_MIN   0x2du
#define HID_USAGE_PUNCT_MAX   0x38u
#define HID_USAGE_CAPS        0x39u
/* HID usage page 0x07 orders RightArrow (0x4F) before LeftArrow (0x50). */
#define HID_USAGE_RIGHT       0x4fu
#define HID_USAGE_LEFT        0x50u
#define HID_USAGE_DOWN        0x51u
#define HID_USAGE_UP          0x52u
#define HID_USAGE_MODIFIER_MIN 0xe0u
#define HID_USAGE_MODIFIER_MAX 0xe7u

#define HID_DESCRIPTOR_INTERFACE 0x04u
#define HID_DESCRIPTOR_ENDPOINT  0x05u
#define HID_CLASS 0x03u
#define HID_SUBCLASS_BOOT 0x01u
#define HID_PROTOCOL_KEYBOARD 0x01u
#define HID_PROTOCOL_MOUSE    0x02u

#define HID_MOUSE_BUTTON_MASK 0x07u

static u8 g_prev[HID_USAGE_SLOTS];
static u8 g_caps;
static u8 g_shift;
static u8 g_ctrl;
static u32 g_mouse_buttons;
static u8 g_mouse_seen;

static const u8 g_digit_plain[10] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0'
};
static const u8 g_digit_shifted[10] = {
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')'
};
static const u8 g_punct_plain[12] = {
    0x2d, 0x3d, 0x5b, 0x5d, 0x5c, 0x00, 0x3b, 0x27, 0x60, 0x2c, 0x2e, 0x2f
};
static const u8 g_punct_shifted[12] = {
    0x5f, 0x2b, 0x7b, 0x7d, 0x7c, 0x00, 0x3a, 0x22, 0x7e, 0x3c, 0x3e, 0x3f
};

void usb_hid_reset(void)
{
    u32 i;

    for (i = 0; i < HID_USAGE_SLOTS; i++)
        g_prev[i] = 0;
    g_caps = 0;
    g_shift = 0;
    g_ctrl = 0;
    g_mouse_buttons = 0;
    g_mouse_seen = 0;
}

int usb_hid_parse_config(const u8 *buf, u32 len, struct usb_hid_endpoint *out)
{
    u32 off = 0;
    u8 kind = USB_HID_KIND_NONE;

    if (!buf || !out || len < 9)
        return -1;

    while (off + 2 <= len) {
        u8 blen = buf[off];
        u8 type = buf[off + 1];

        if (blen < 2 || off + blen > len)
            return -1;
        if (type == HID_DESCRIPTOR_INTERFACE) {
            kind = USB_HID_KIND_NONE;
            if (blen >= 9 && buf[off + 5] == HID_CLASS &&
                buf[off + 6] == HID_SUBCLASS_BOOT) {
                if (buf[off + 7] == HID_PROTOCOL_KEYBOARD)
                    kind = USB_HID_KIND_KEYBOARD;
                else if (buf[off + 7] == HID_PROTOCOL_MOUSE)
                    kind = USB_HID_KIND_MOUSE;
            }
        } else if (type == HID_DESCRIPTOR_ENDPOINT && kind != USB_HID_KIND_NONE) {
            u8 attr = buf[off + 3];
            u8 addr = buf[off + 2];

            if ((attr & 0x03u) == 0x03u && (addr & 0x80u)) {
                out->ep_addr = addr;
                out->ep_num = (u8)(addr & 0x0fu);
                out->in = 1;
                out->kind = kind;
                out->max_packet = (u16)((buf[off + 4] | ((u16)buf[off + 5] << 8)) & 0x07ffu);
                out->interval = buf[off + 6];
                return 0;
            }
        }
        off += blen;
    }
    return -1;
}

u32 usb_config_interface_class(const u8 *buf, u32 len, u32 index)
{
    u32 off = 0;
    u32 seen = 0;

    if (!buf || len < 9)
        return 0xffu;
    while (off + 2u <= len) {
        u32 blen = buf[off];
        u32 type = buf[off + 1u];

        if (blen < 2u || off + blen > len)
            break;
        if (type == HID_DESCRIPTOR_INTERFACE && blen >= 9u) {
            if (seen == index)
                return (u32)buf[off + 5u];
            seen++;
        }
        off += blen;
    }
    return 0xffu;
}

static int usage_was_down(u8 usage)
{
    u32 i;

    for (i = 0; i < HID_USAGE_SLOTS; i++) {
        if (g_prev[i] == usage)
            return 1;
    }
    return 0;
}

static int hid_map_usage(u8 usage)
{
    u8 code = 0;

    if (usage >= HID_USAGE_MIN && usage <= HID_USAGE_LETTER_END) {
        code = (u8)('a' + (usage - HID_USAGE_MIN));
        if (g_shift != g_caps)
            code = (u8)(code - 32);
    } else if (usage >= HID_USAGE_DIGIT_MIN && usage <= HID_USAGE_DIGIT_MAX) {
        u8 idx = (u8)(usage - HID_USAGE_DIGIT_MIN);

        code = g_shift ? g_digit_shifted[idx] : g_digit_plain[idx];
    } else if (usage >= HID_USAGE_PUNCT_MIN && usage <= HID_USAGE_PUNCT_MAX) {
        u8 idx = (u8)(usage - HID_USAGE_PUNCT_MIN);

        code = g_shift ? g_punct_shifted[idx] : g_punct_plain[idx];
    } else {
        switch (usage) {
        case HID_USAGE_ENTER:
            code = 0x0a;
            break;
        case HID_USAGE_ESC:
            code = 0x1b;
            break;
        case HID_USAGE_BACKSPACE:
            code = 0x7f;
            break;
        case HID_USAGE_TAB:
            code = 0x09;
            break;
        case HID_USAGE_SPACE:
            code = ' ';
            break;
        case HID_USAGE_UP:
            code = (u8)INPUT_KEY_UP;
            break;
        case HID_USAGE_DOWN:
            code = (u8)INPUT_KEY_DOWN;
            break;
        case HID_USAGE_LEFT:
            code = (u8)INPUT_KEY_LEFT;
            break;
        case HID_USAGE_RIGHT:
            code = (u8)INPUT_KEY_RIGHT;
            break;
        default:
            return 0;
        }
    }

    if (!code)
        return 0;
    /* Ctrl folds the same way as the PS/2 keymap in kernel/input/input.c. */
    if (g_ctrl) {
        if (code >= 'a' && code <= 'z')
            return (int)(code - 'a' + 1);
        if (code >= 'A' && code <= 'Z')
            return (int)(code - 'A' + 1);
        if (code == ' ' || code == '@')
            return 0;
        if (code >= '[' && code <= '_')
            return (int)(code - '@');
    }
    return (int)code;
}

int usb_hid_boot_decode(const u8 report[USB_HID_REPORT_BYTES])
{
    u8 mods;
    u8 pressed[HID_USAGE_SLOTS];
    u32 i;
    int ch = 0;

    if (!report)
        return 0;

    mods = report[0];
    g_shift = (mods & HID_SHIFT_MASK) ? 1 : 0;
    g_ctrl = (mods & HID_CTRL_MASK) ? 1 : 0;
    (void)HID_MOD_LALT;
    (void)HID_MOD_RALT;

    for (i = 0; i < HID_USAGE_SLOTS; i++)
        pressed[i] = report[1 + i];

    for (i = 0; i < HID_USAGE_SLOTS; i++) {
        if (pressed[i] == HID_USAGE_CAPS && !usage_was_down(HID_USAGE_CAPS)) {
            g_caps ^= 1u;
            break;
        }
    }

    for (i = 0; i < HID_USAGE_SLOTS; i++) {
        u8 usage = pressed[i];

        if (usage < HID_USAGE_MIN)
            continue;
        if (usage == HID_USAGE_CAPS)
            continue;
        if (usage >= HID_USAGE_MODIFIER_MIN && usage <= HID_USAGE_MODIFIER_MAX)
            continue;
        if (usage_was_down(usage))
            continue;
        if (!ch)
            ch = hid_map_usage(usage);
    }

    for (i = 0; i < HID_USAGE_SLOTS; i++)
        g_prev[i] = pressed[i];

    return ch;
}

static i32 hid_sign8(u8 v)
{
    return (i32)((i8)v);
}

int usb_hid_mouse_decode(const u8 *report, u32 len, struct usb_hid_pointer *out)
{
    u32 buttons;
    i32 dx;
    i32 dy;
    i32 wheel = 0;

    if (!report || !out || len < 3u)
        return -1;
    if (len > USB_HID_MOUSE_REPORT_BYTES)
        len = USB_HID_MOUSE_REPORT_BYTES;

    buttons = (u32)(report[0] & HID_MOUSE_BUTTON_MASK);
    dx = hid_sign8(report[1]);
    dy = hid_sign8(report[2]);
    if (len >= 4u)
        wheel = hid_sign8(report[3]);

    out->dx = dx;
    out->dy = dy;
    out->wheel = wheel;
    out->buttons = buttons;
    /* The first report only establishes the button baseline, just like the
       keyboard ignores its initial all-zero report. */
    out->changes = g_mouse_seen ? (buttons ^ g_mouse_buttons) : 0u;
    g_mouse_buttons = buttons;
    g_mouse_seen = 1;

    if (dx || dy || wheel || out->changes)
        return 1;
    return 0;
}
