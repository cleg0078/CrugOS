/* USB Audio Class 1 and 2 playback (phase 5C).

   The xHCI driver hands us the configuration descriptor of an audio device;
   we parse the AudioControl and AudioStreaming interfaces, select the
   alternate setting that carries the isochronous endpoint, program the
   sample rate and then keep the endpoint fed from audio.device. Capture is
   supported by the parser but QEMU's usb-audio exposes playback only on
   hosts without a capture-capable backend, so the boot self-test reports
   "audio capture unavailable" there. */
#include <crug/types.h>
#include <crug/kernel.h>
#include <crug/audio.h>
#include <crug/usb.h>
#include <crug/timer.h>
#include <crug/cpu.h>

#define UAC_DT_CONFIG          0x02u
#define UAC_DT_INTERFACE       0x04u
#define UAC_DT_ENDPOINT        0x05u
#define UAC_DT_CS_INTERFACE    0x24u
#define UAC_DT_CS_ENDPOINT     0x25u

#define UAC_CLASS_AUDIO        0x01u
#define UAC_SUBCLASS_CONTROL   0x01u
#define UAC_SUBCLASS_STREAM    0x02u

#define UAC_AC_HEADER          0x01u
#define UAC_AC_INPUT_TERMINAL  0x02u
#define UAC_AC_OUTPUT_TERMINAL 0x03u
#define UAC_AC_FEATURE_UNIT    0x06u
#define UAC_AC_CLOCK_SOURCE    0x0au
#define UAC_AS_GENERAL         0x01u
#define UAC_AS_FORMAT_TYPE     0x02u

#define UAC_FORMAT_TYPE_I      0x01u
#define UAC_REQUEST_SET_CUR    0x01u
#define UAC_REQUEST_SET_INTERFACE 0x0bu
#define UAC_CS_VOLUME          0x02u
#define UAC_CS_MUTE            0x01u
#define UAC_CS_SAMPLING_FREQ   0x01u
#define UAC_MAX_CTRL_CHANNELS  2u

struct uac_card {
    int used;
    u32 version;            /* 1 or 2 */
    u32 slot;
    u32 port;
    u32 speed;
    u32 iface;              /* AudioStreaming interface number */
    u32 alt;                /* alternate setting with the endpoint */
    u32 ac_iface;           /* AudioControl interface number */
    u32 clock_id;           /* UAC2 clock source entity */
    u32 feature_unit;       /* feature unit entity, 0 when absent */
    u32 volume_mask;        /* bit per channel (bit 0 = master) */
    u32 mute_mask;
    u32 rate;
    u32 channels;
    u32 bits;
    u8 ep_out;
    u8 ep_in;
    u16 out_packet;
    u8 out_interval;
    u16 in_packet;
    u8 in_interval;
    int started;
    int capture;
    u32 out_sent;
    u32 out_done;
    u32 in_sent;
    u32 in_done;
};

static struct uac_card g_card;

/* USB Audio 1.0 puts the entity id in the low byte and the interface number in
   the high byte of wIndex. Some hosts (QEMU's usb-audio among them) expect the
   two bytes swapped, so the driver starts with the specified layout and
   remembers the one that worked for every later request. */
static u32 g_ctrl_style;

static u16 uac_feature_index(u32 style)
{
    if (style == 2u)
        return (u16)((g_card.feature_unit << 8) | (g_card.ac_iface & 0xffu));
    return (u16)(g_card.feature_unit | ((g_card.ac_iface & 0xffu) << 8));
}

static int uac_feature_control(u16 value, u8 *data, u16 len)
{
    if (g_ctrl_style != 2u) {
        if (usb_control(g_card.slot, 0x21u, UAC_REQUEST_SET_CUR, value,
                        uac_feature_index(1u), data, len) == 0) {
            g_ctrl_style = 1u;
            return 0;
        }
        g_ctrl_style = 2u;
    }
    return usb_control(g_card.slot, 0x21u, UAC_REQUEST_SET_CUR, value,
                       uac_feature_index(2u), data, len);
}

static u16 uac_get16(const u8 *p)
{
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static u32 uac_get32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* Pick the sample rate we want out of a format type I descriptor. */
static u32 uac_pick_rate(const u8 *fmt, u32 blen)
{
    u32 freq_type;
    u32 count;
    u32 first = 0;
    u32 i;
    u32 size = g_card.version >= 2u ? 4u : 3u;

    if (blen < 8)
        return 0;
    freq_type = fmt[7];
    if (freq_type == 0) {
        /* Continuous: tLower and tUpper follow. */
        if (blen < 8u + 2u * size)
            return 0;
        first = size == 4u ? uac_get32(fmt + 8) : (u32)(fmt[8] | (fmt[9] << 8) | ((u32)fmt[10] << 16));
        if (first < AUDIO_RATE_DEFAULT)
            first = AUDIO_RATE_DEFAULT;
        return first;
    }
    count = freq_type;
    if (count > 8u || blen < 8u + count * size)
        count = (blen - 8u) / size;
    for (i = 0; i < count; i++) {
        const u8 *p = fmt + 8u + i * size;
        u32 rate = size == 4u ? uac_get32(p) : (u32)(p[0] | (p[1] << 8) | ((u32)p[2] << 16));

        if (i == 0)
            first = rate;
        if (rate == AUDIO_RATE_DEFAULT)
            return rate;
    }
    return first;
}

static void uac_controls(const u8 *unit, u32 blen, u32 start, u32 size)
{
    u32 count;
    u32 i;

    if (blen <= start + 1u)
        return;
    count = (blen - 1u - start) / size;
    if (count > UAC_MAX_CTRL_CHANNELS + 1u)
        count = UAC_MAX_CTRL_CHANNELS + 1u;
    for (i = 0; i < count; i++) {
        const u8 *p = unit + start + i * size;
        u32 controls = size == 4u ? uac_get32(p) : uac_get16(p);

        if (controls & UAC_CS_MUTE)
            g_card.mute_mask |= 1u << i;
        if (controls & UAC_CS_VOLUME)
            g_card.volume_mask |= 1u << i;
    }
}

static void uac_cs_interface(const u8 *d, u32 blen, int in_stream)
{
    u32 subtype;

    if (blen < 3)
        return;
    subtype = d[2];
    if (!in_stream) {
        switch (subtype) {
        case UAC_AC_HEADER:
            if (blen >= 5) {
                u16 bcd = uac_get16(d + 3);

                g_card.version = bcd >= 0x0200u ? 2u : 1u;
            }
            break;
        case UAC_AC_INPUT_TERMINAL:
            if (g_card.channels == 0) {
                if (g_card.version >= 2u && blen >= 9)
                    g_card.channels = d[8];
                else if (blen >= 8)
                    g_card.channels = d[7];
            }
            break;
        case UAC_AC_OUTPUT_TERMINAL:
            break;
        case UAC_AC_FEATURE_UNIT:
            if (blen >= 7) {
                if (g_card.feature_unit == 0) {
                    g_card.feature_unit = d[3];
                    if (g_card.version >= 2u)
                        uac_controls(d, blen, 5, 4);
                    else
                        uac_controls(d, blen, 7, d[5] ? d[5] : 2u);
                }
            }
            break;
        case UAC_AC_CLOCK_SOURCE:
            if (blen >= 4 && g_card.clock_id == 0)
                g_card.clock_id = d[3];
            break;
        default:
            break;
        }
        return;
    }
    switch (subtype) {
    case UAC_AS_GENERAL:
        break;
    case UAC_AS_FORMAT_TYPE:
        if (blen >= 8 && d[3] == UAC_FORMAT_TYPE_I) {
            u32 rate = uac_pick_rate(d, blen);

            if (g_card.channels == 0)
                g_card.channels = d[4];
            if (g_card.bits == 0)
                g_card.bits = d[6];
            if (g_card.rate == 0)
                g_card.rate = rate;
        }
        break;
    default:
        break;
    }
}

static int uac_parse(const u8 *buf, u32 len)
{
    u32 off = 0;
    int in_stream = 0;

    while (off + 2u <= len) {
        u32 blen = buf[off];
        u32 type = buf[off + 1u];

        if (blen < 2u || off + blen > len)
            break;
        if (type == UAC_DT_INTERFACE && blen >= 9) {
            in_stream = 0;
            if (buf[off + 5u] == UAC_CLASS_AUDIO && buf[off + 6u] == UAC_SUBCLASS_STREAM) {
                in_stream = 1;
                if (g_card.iface == 0 && buf[off + 3u] > 0 && buf[off + 4u] > 0) {
                    g_card.iface = buf[off + 2u];
                    g_card.alt = buf[off + 3u];
                }
            } else if (buf[off + 5u] == UAC_CLASS_AUDIO && buf[off + 6u] == UAC_SUBCLASS_CONTROL) {
                if (g_card.ac_iface == 0)
                    g_card.ac_iface = buf[off + 2u];
            }
        } else if (type == UAC_DT_CS_INTERFACE) {
            uac_cs_interface(buf + off, blen, in_stream);
        } else if (type == UAC_DT_ENDPOINT && blen >= 7 && in_stream) {
            u32 attr = buf[off + 3u];
            u8 addr = buf[off + 2u];
            u16 packet = (u16)(buf[off + 4u] | ((u16)buf[off + 5u] << 8)) & 0x7ffu;

            if ((attr & 0x03u) == 0x01u) {
                if (packet == 0)
                    packet = USB_ISOC_PACKET_MAX;
                if (addr & 0x80u) {
                    if (g_card.ep_in == 0) {
                        g_card.ep_in = addr;
                        g_card.in_packet = packet;
                        g_card.in_interval = buf[off + 6u];
                    }
                } else if (g_card.ep_out == 0) {
                    g_card.ep_out = addr;
                    g_card.out_packet = packet;
                    g_card.out_interval = buf[off + 6u];
                }
            }
        }
        off += blen;
    }
    return 0;
}

void usb_audio_attach(u32 slot, u32 port, u32 speed, const u8 *config, u32 config_len)
{
    u32 i;

    for (i = 0; i < sizeof(g_card); i++)
        ((u8 *)&g_card)[i] = 0;
    if (!config || config_len < 9u) {
        kprintf("CRUGOS: usb audio has no configuration\n");
        return;
    }
    g_card.slot = slot;
    g_card.port = port;
    g_card.speed = speed;
    g_card.version = 1;
    uac_parse(config, config_len);
    if (g_card.ep_out == 0 && g_card.ep_in == 0) {
        kprintf("CRUGOS: usb audio has no streaming endpoint\n");
        return;
    }
    if (g_card.iface == 0 || g_card.alt == 0) {
        kprintf("CRUGOS: usb audio has no alternate setting\n");
        return;
    }
    if (g_card.rate == 0)
        g_card.rate = AUDIO_RATE_DEFAULT;
    if (g_card.channels == 0 || g_card.channels > AUDIO_CHANNELS)
        g_card.channels = AUDIO_CHANNELS;
    if (g_card.bits == 0 || g_card.bits > AUDIO_BITS_DEFAULT)
        g_card.bits = AUDIO_BITS_DEFAULT;
    g_card.used = 1;
    kprintf("CRUGOS: usb audio attached (uac%u, %u ch, %u-bit, %u Hz, iface %u alt %u)\n",
            (unsigned)g_card.version, (unsigned)g_card.channels,
            (unsigned)g_card.bits, (unsigned)g_card.rate,
            (unsigned)g_card.iface, (unsigned)g_card.alt);
}

void usb_audio_detach(u32 slot)
{
    if (!g_card.used || g_card.slot != slot)
        return;
    for (u32 i = 0; i < sizeof(g_card); i++)
        ((u8 *)&g_card)[i] = 0;
}

int usb_audio_ready(void)
{
    return g_card.used;
}

const char *usb_audio_class_name(void)
{
    if (!g_card.used)
        return "none";
    return g_card.version >= 2u ? "uac2" : "uac1";
}

u32 usb_audio_rate(void)
{
    return g_card.used ? g_card.rate : 0;
}

u32 usb_audio_channels(void)
{
    return g_card.used ? g_card.channels : 0;
}

u32 usb_audio_bits(void)
{
    return g_card.used ? g_card.bits : 0;
}

static void uac_fill_out(void)
{
    u32 capacity;
    u32 done;

    if (!g_card.started || !g_card.ep_out)
        return;
    capacity = usb_isoc_packets(g_card.slot, g_card.ep_out);
    if (capacity == 0)
        return;
    /* Only a successful (or short) completion frees a slot: an underrun event
       is generated by a kick that found the ring empty and owns no packet. */
    done = (u32)usb_isoc_done(g_card.slot, g_card.ep_out);
    while ((u32)(g_card.out_sent - done) < capacity) {
        u32 index = g_card.out_sent % capacity;
        void *buf = usb_isoc_buf(g_card.slot, g_card.ep_out, index);
        u32 frames = (u32)g_card.out_packet / AUDIO_FRAME_BYTES;
        u32 bytes;

        if (!buf || frames == 0)
            return;
        audio_read((i16 *)buf, frames);
        bytes = frames * AUDIO_FRAME_BYTES;
        if (usb_isoc_submit(g_card.slot, g_card.ep_out, index, bytes) != 0)
            return;
        g_card.out_sent++;
    }
}

int usb_audio_start(u32 rate, u32 channels, u32 bits)
{
    u8 data[4];
    u32 i;
    int failed = 0;

    (void)bits;
    if (!g_card.used)
        return -1;
    if (g_card.started)
        return 0;
    if (channels && channels < g_card.channels)
        g_card.channels = channels;
    if (usb_control(g_card.slot, 0x01u, UAC_REQUEST_SET_INTERFACE,
                    (u16)g_card.alt, (u16)g_card.iface, 0, 0) != 0) {
        kprintf("CRUGOS: usb audio alt setting failed\n");
        return -1;
    }
    for (i = 0; i < 4u; i++)
        data[i] = (u8)((rate >> (8u * i)) & 0xffu);
    if (g_card.version >= 2u) {
        if (usb_control(g_card.slot, 0x21u, UAC_REQUEST_SET_CUR,
                        (u16)(UAC_CS_SAMPLING_FREQ << 8),
                        (u16)(g_card.clock_id | (g_card.ac_iface << 8)),
                        data, 4) != 0)
            failed = 1;
    } else {
        if (usb_control(g_card.slot, 0x22u, UAC_REQUEST_SET_CUR,
                        (u16)(UAC_CS_SAMPLING_FREQ << 8),
                        (u16)(g_card.ep_out | (g_card.iface << 8)),
                        data, 3) != 0)
            failed = 1;
    }
    if (failed)
        kprintf("CRUGOS: usb audio rate request failed\n");
    if (g_card.ep_out &&
        usb_isoc_open(g_card.slot, g_card.ep_out, g_card.out_packet,
                      g_card.out_interval) != 0)
        return -1;
    if (g_card.ep_in &&
        usb_isoc_open(g_card.slot, g_card.ep_in, g_card.in_packet,
                      g_card.in_interval) != 0)
        return -1;
    g_card.started = 1;
    g_card.out_sent = 0;
    g_card.in_sent = 0;
    g_card.in_done = 0;
    uac_fill_out();
    return 0;
}

void usb_audio_stop(void)
{
    if (!g_card.started)
        return;
    g_card.started = 0;
    g_card.capture = 0;
}

int usb_audio_capture_available(void)
{
    return g_card.used && g_card.ep_in != 0;
}

int usb_audio_capture_start(void)
{
    u32 capacity;
    u32 i;

    if (!g_card.started || !g_card.ep_in)
        return -1;
    capacity = usb_isoc_packets(g_card.slot, g_card.ep_in);
    if (capacity == 0)
        return -1;
    for (i = 0; i < capacity; i++) {
        if (usb_isoc_submit(g_card.slot, g_card.ep_in, i, g_card.in_packet) != 0)
            return -1;
        g_card.in_sent++;
    }
    g_card.capture = 1;
    return 0;
}

static int uac_volume_channel(u32 channel, i32 volume)
{
    u8 data[2];

    data[0] = (u8)((u16)(i16)volume & 0xffu);
    data[1] = (u8)(((u16)(i16)volume >> 8) & 0xffu);
    return uac_feature_control((u16)((UAC_CS_VOLUME << 8) | (channel & 0xffu)),
                               data, 2);
}

int usb_audio_set_volume(i32 volume)
{
    u32 ch;
    int any = 0;

    if (!g_card.used || !g_card.feature_unit)
        return -1;
    if (volume > AUDIO_VOLUME_MAX)
        volume = AUDIO_VOLUME_MAX;
    if (volume < AUDIO_VOLUME_MIN)
        volume = AUDIO_VOLUME_MIN;
    for (ch = 1; ch <= g_card.channels && ch <= UAC_MAX_CTRL_CHANNELS; ch++) {
        if (!(g_card.volume_mask & (1u << ch)))
            continue;
        if (uac_volume_channel(ch, volume) == 0)
            any = 1;
    }
    if (!any && (g_card.volume_mask & 1u))
        any = uac_volume_channel(0, volume) == 0;
    return any ? 0 : -1;
}

int usb_audio_set_mute(u32 mute)
{
    u8 data = mute ? 1u : 0u;
    u32 ch;

    if (!g_card.used || !g_card.feature_unit)
        return -1;
    for (ch = 0; ch <= g_card.channels && ch <= UAC_MAX_CTRL_CHANNELS; ch++) {
        if (!(g_card.mute_mask & (1u << ch)))
            continue;
        if (uac_feature_control((u16)((UAC_CS_MUTE << 8) | ch), &data, 1) != 0)
            return -1;
    }
    return 0;
}

void usb_audio_poll(void)
{
    if (!g_card.started)
        return;
    if (g_card.ep_out) {
        u32 capacity = usb_isoc_packets(g_card.slot, g_card.ep_out);
        u32 done = (u32)usb_isoc_done(g_card.slot, g_card.ep_out);
        u32 frames = (u32)g_card.out_packet / AUDIO_FRAME_BYTES;

        /* Every success is one packet played by the device, so the frame
           counter only moves forward when a real completion arrives. */
        if (capacity && frames && done > g_card.out_done) {
            audio_played((done - g_card.out_done) * frames);
            g_card.out_done = done;
        }
    }
    uac_fill_out();
    if (g_card.capture && g_card.ep_in) {
        u32 capacity = usb_isoc_packets(g_card.slot, g_card.ep_in);
        u32 done = (u32)usb_isoc_done(g_card.slot, g_card.ep_in);

        while (capacity && g_card.in_done < done) {
            u32 index = g_card.in_done % capacity;
            u32 frames = (u32)g_card.in_packet / AUDIO_FRAME_BYTES;

            if (frames)
                audio_captured(frames);
            (void)usb_isoc_submit(g_card.slot, g_card.ep_in, index, g_card.in_packet);
            g_card.in_sent++;
            g_card.in_done++;
        }
    }
}
