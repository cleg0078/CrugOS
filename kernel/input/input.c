/* input.device: PS/2 set-1 scancode decoder feeding a byte queue, plus the
   pointer device fed by the USB HID boot mouse. */
#include <crug/types.h>
#include <crug/input.h>

#define INPUT_QUEUE_BYTES 64u

static u8 g_queue[INPUT_QUEUE_BYTES];
static u32 g_head;
static u32 g_tail;
static u32 g_count;

static void (*g_poll)(void);

static u8 g_extended;
static u8 g_shift;
static u8 g_ctrl;
static u8 g_alt;
static u8 g_caps;

static i32 g_ptr_dx;
static i32 g_ptr_dy;
static i32 g_ptr_wheel;
static u32 g_ptr_buttons;
static u32 g_ptr_changes;
static u64 g_ptr_events;

static void input_push(u8 c)
{
    if (g_count >= INPUT_QUEUE_BYTES)
        return;
    g_queue[g_tail] = c;
    g_tail = (g_tail + 1u) % INPUT_QUEUE_BYTES;
    g_count++;
}

void input_char(u8 c)
{
    input_push(c);
}

void input_pointer_event(u32 buttons, i32 dx, i32 dy, i32 wheel)
{
    g_ptr_dx += dx;
    g_ptr_dy += dy;
    g_ptr_wheel += wheel;
    g_ptr_changes |= buttons ^ g_ptr_buttons;
    g_ptr_buttons = buttons;
    g_ptr_events++;
}

int input_pointer_read(struct input_pointer *out)
{
    if (!out)
        return 0;
    if (!g_ptr_dx && !g_ptr_dy && !g_ptr_wheel && !g_ptr_changes)
        return 0;
    out->dx = g_ptr_dx;
    out->dy = g_ptr_dy;
    out->wheel = g_ptr_wheel;
    out->buttons = g_ptr_buttons;
    out->changes = g_ptr_changes;
    g_ptr_dx = 0;
    g_ptr_dy = 0;
    g_ptr_wheel = 0;
    g_ptr_changes = 0;
    return 1;
}

void input_pointer_reset(void)
{
    g_ptr_dx = 0;
    g_ptr_dy = 0;
    g_ptr_wheel = 0;
    g_ptr_buttons = 0;
    g_ptr_changes = 0;
    g_ptr_events = 0;
}

u64 input_pointer_count(void)
{
    return g_ptr_events;
}

void input_set_poll(void (*fn)(void))
{
    g_poll = fn;
}

void input_poll(void)
{
    if (g_poll)
        g_poll();
}

void input_init(void)
{
    g_head = 0;
    g_tail = 0;
    g_count = 0;
    g_extended = 0;
    g_shift = 0;
    g_ctrl = 0;
    g_alt = 0;
    g_caps = 0;
    input_pointer_reset();
}

int input_try_getc(void)
{
    u8 c;

    if (g_poll)
        g_poll();
    if (g_count == 0)
        return -1;
    c = g_queue[g_head];
    g_head = (g_head + 1u) % INPUT_QUEUE_BYTES;
    g_count--;
    return (int)c;
}

/* Unshifted and shifted glyphs by set-1 make code. 0 means none. */
static const char g_unshifted[0x59] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '-', '=', 0, 0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    'o', 'p', '[', ']', 0, 0, 'a', 's', 'd', 'f', 'g', 'h',
    'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0
};

static const char g_shifted[0x59] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '_', '+', 0, 0, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{', '}', 0, 0, 'A', 'S', 'D', 'F', 'G', 'H',
    'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0
};

static int input_is_letter_code(u8 code)
{
    if (code >= 0x10 && code <= 0x19)
        return 1;
    if (code >= 0x1E && code <= 0x26)
        return 1;
    if (code >= 0x2C && code <= 0x32)
        return 1;
    return 0;
}

static char input_ascii(u8 code)
{
    char c;

    if (code >= sizeof(g_unshifted))
        return 0;
    c = g_shift ? g_shifted[code] : g_unshifted[code];
    if (g_caps && input_is_letter_code(code)) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    if (g_ctrl) {
        if (c >= 'a' && c <= 'z')
            return (char)(c - 'a' + 1);
        if (c >= 'A' && c <= 'Z')
            return (char)(c - 'A' + 1);
        if (c == ' ' || c == '@')
            return 0;
        if (c >= '[' && c <= '_')
            return (char)(c - '@');
    }
    return c;
}

static void input_key_make(u8 code)
{
    char c;

    switch (code) {
    case 0x1C:
        input_push('\n');
        return;
    case 0x0E:
        input_push(0x7F);
        return;
    case 0x0F:
        input_push('\t');
        return;
    case 0x01:
        input_push(0x1B);
        return;
    default:
        break;
    }
    /* Set-1 scancodes: F1..F10 are 0x3B..0x44, F11 is 0x57, F12 is 0x58. */
    if (code >= 0x3B && code <= 0x44) {
        input_push((u8)(INPUT_KEY_F1 + (u8)(code - 0x3B)));
        return;
    }
    if (code == 0x57) {
        input_push((u8)INPUT_KEY_F11);
        return;
    }
    if (code == 0x58) {
        input_push((u8)INPUT_KEY_F12);
        return;
    }
    c = input_ascii(code);
    if (c != 0)
        input_push((u8)c);
}

static void input_modifier(u8 code, int pressed)
{
    switch (code) {
    case 0x2A:
    case 0x36:
        g_shift = (u8)pressed;
        break;
    case 0x1D:
        g_ctrl = (u8)pressed;
        break;
    case 0x38:
        g_alt = (u8)pressed;
        break;
    case 0x3A:
        if (pressed)
            g_caps = (u8)!g_caps;
        break;
    default:
        break;
    }
}

void input_scancode(u8 scan)
{
    u8 code;
    int pressed;

    if (scan == 0xFA || scan == 0xEE)
        return; /* keyboard ACK / echo */
    if (scan == 0xE0) {
        g_extended = 1;
        return;
    }
    if (scan == 0xE1)
        return; /* Pause/Break prefix: ignored */

    pressed = (scan & 0x80u) == 0;
    code = (u8)(scan & 0x7Fu);

    if (g_extended) {
        g_extended = 0;
        if (code == 0x1D || code == 0x38) {
            input_modifier(code, pressed);
            return;
        }
        if (!pressed)
            return; /* the arrow break codes carry no information */
        /* E0 arrows become INPUT_KEY_* codes (phase 6D); Home/End/Insert/Delete
           stay unmapped until a terminal consumer needs them. */
        switch (code) {
        case 0x48:
            input_push((u8)INPUT_KEY_UP);
            break;
        case 0x50:
            input_push((u8)INPUT_KEY_DOWN);
            break;
        case 0x4B:
            input_push((u8)INPUT_KEY_LEFT);
            break;
        case 0x4D:
            input_push((u8)INPUT_KEY_RIGHT);
            break;
        default:
            break;
        }
        return;
    }

    if (code == 0x2A || code == 0x36 || code == 0x1D || code == 0x38 ||
        code == 0x3A) {
        input_modifier(code, pressed);
        return;
    }
    if (pressed)
        input_key_make(code);
}
