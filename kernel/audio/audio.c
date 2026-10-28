/* audio.device: a 16-bit stereo PCM ring, a tone generator and the boot
   self-test (phase 5C). The USB audio driver drains the ring into
   isochronous packets; the mixer sits between the ring and the hardware. */
#include <crug/types.h>
#include <crug/kernel.h>
#include <crug/audio.h>
#include <crug/timer.h>
#include <crug/cpu.h>
#include <crug/string.h>
#include <crug/usb.h>

#define AUDIO_SELFTEST_FREQ   440u
#define AUDIO_SELFTEST_MS     5000u
#define AUDIO_CAPTURE_MS      500u

static i16 g_ring[AUDIO_RING_FRAMES * AUDIO_CHANNELS];
static u32 g_head;               /* next frame to read */
static u32 g_count;              /* frames currently queued */
static u64 g_played;
static u64 g_captured;
static u32 g_dropped;
static u32 g_rate = AUDIO_RATE_DEFAULT;
static u32 g_rate_requested;
static u32 g_seconds;
static i32 g_volume = -2560;     /* -10 dB */

static const i16 g_sine[360] = {
         0,    572,   1144,   1715,   2286,   2856,   3425,   3993,   4560,   5126,
      5690,   6252,   6813,   7371,   7927,   8481,   9032,   9580,  10126,  10668,
     11207,  11743,  12275,  12803,  13328,  13848,  14364,  14876,  15383,  15886,
     16383,  16876,  17364,  17846,  18323,  18794,  19260,  19720,  20173,  20621,
     21062,  21497,  21925,  22347,  22762,  23170,  23571,  23964,  24351,  24730,
     25101,  25465,  25821,  26169,  26509,  26841,  27165,  27481,  27788,  28087,
     28377,  28659,  28932,  29196,  29451,  29697,  29934,  30162,  30381,  30591,
     30791,  30982,  31163,  31335,  31498,  31650,  31794,  31927,  32051,  32165,
     32269,  32364,  32448,  32523,  32587,  32642,  32687,  32722,  32747,  32762,
     32767,  32762,  32747,  32722,  32687,  32642,  32587,  32523,  32448,  32364,
     32269,  32165,  32051,  31927,  31794,  31650,  31498,  31335,  31163,  30982,
     30791,  30591,  30381,  30162,  29934,  29697,  29451,  29196,  28932,  28659,
     28377,  28087,  27788,  27481,  27165,  26841,  26509,  26169,  25821,  25465,
     25101,  24730,  24351,  23964,  23571,  23170,  22762,  22347,  21925,  21497,
     21062,  20621,  20173,  19720,  19260,  18794,  18323,  17846,  17364,  16876,
     16383,  15886,  15383,  14876,  14364,  13848,  13328,  12803,  12275,  11743,
     11207,  10668,  10126,   9580,   9032,   8481,   7927,   7371,   6813,   6252,
      5690,   5126,   4560,   3993,   3425,   2856,   2286,   1715,   1144,    572,
         0,   -572,  -1144,  -1715,  -2286,  -2856,  -3425,  -3993,  -4560,  -5126,
     -5690,  -6252,  -6813,  -7371,  -7927,  -8481,  -9032,  -9580, -10126, -10668,
    -11207, -11743, -12275, -12803, -13328, -13848, -14364, -14876, -15383, -15886,
    -16384, -16876, -17364, -17846, -18323, -18794, -19260, -19720, -20173, -20621,
    -21062, -21497, -21925, -22347, -22762, -23170, -23571, -23964, -24351, -24730,
    -25101, -25465, -25821, -26169, -26509, -26841, -27165, -27481, -27788, -28087,
    -28377, -28659, -28932, -29196, -29451, -29697, -29934, -30162, -30381, -30591,
    -30791, -30982, -31163, -31335, -31498, -31650, -31794, -31927, -32051, -32165,
    -32269, -32364, -32448, -32523, -32587, -32642, -32687, -32722, -32747, -32762,
    -32767, -32762, -32747, -32722, -32687, -32642, -32587, -32523, -32448, -32364,
    -32269, -32165, -32051, -31927, -31794, -31650, -31498, -31335, -31163, -30982,
    -30791, -30591, -30381, -30162, -29934, -29697, -29451, -29196, -28932, -28659,
    -28377, -28087, -27788, -27481, -27165, -26841, -26509, -26169, -25821, -25465,
    -25101, -24730, -24351, -23964, -23571, -23170, -22762, -22347, -21925, -21497,
    -21062, -20621, -20173, -19720, -19260, -18794, -18323, -17846, -17364, -16876,
    -16384, -15886, -15383, -14876, -14364, -13848, -13328, -12803, -12275, -11743,
    -11207, -10668, -10126,  -9580,  -9032,  -8481,  -7927,  -7371,  -6813,  -6252,
     -5690,  -5126,  -4560,  -3993,  -3425,  -2856,  -2286,  -1715,  -1144,   -572,
};

static int audio_prefix(const char *tok, u64 len, const char *prefix, u64 plen)
{
    return len >= plen && memcmp(tok, prefix, (usize)plen) == 0;
}

static int audio_parse_int(const char *text, u64 len, int *out)
{
    u64 i = 0;
    int negative = 0;
    int value = 0;

    if (i < len && (text[i] == '-' || text[i] == '+')) {
        negative = text[i] == '-';
        i++;
    }
    if (i >= len)
        return -1;
    for (; i < len; i++) {
        if (text[i] < '0' || text[i] > '9')
            return -1;
        value = value * 10 + (text[i] - '0');
        if (value > 1000000)
            return -1;
    }
    *out = negative ? -value : value;
    return 0;
}

static void audio_cmdline_token(const char *tok, u64 len)
{
    int value;

    if (audio_prefix(tok, len, "audio.rate=", 11) &&
        audio_parse_int(tok + 11, len - 11u, &value) == 0 &&
        value >= 8000 && value <= 192000) {
        g_rate_requested = (u32)value;
    } else if (audio_prefix(tok, len, "audio.volume=", 13) &&
               audio_parse_int(tok + 13, len - 13u, &value) == 0) {
        if (value > AUDIO_VOLUME_MAX)
            value = AUDIO_VOLUME_MAX;
        if (value < AUDIO_VOLUME_MIN)
            value = AUDIO_VOLUME_MIN;
        g_volume = value;
    } else if (audio_prefix(tok, len, "audio.seconds=", 14) &&
               audio_parse_int(tok + 14, len - 14u, &value) == 0 &&
               value >= 0 && value <= 5) {
        g_seconds = (u32)value;
    }
}

void audio_cmdline(const boot_info *bi)
{
    const char *p;
    u64 len;
    u64 i = 0;

    if (!bi)
        return;
    p = bi->command_line;
    len = bi->command_line_len;
    if (!p || len == 0)
        return;
    while (i < len) {
        u64 start;

        while (i < len && (p[i] == ' ' || p[i] == '\t'))
            i++;
        start = i;
        while (i < len && p[i] != ' ' && p[i] != '\t')
            i++;
        if (i > start)
            audio_cmdline_token(p + start, i - start);
    }
}

int audio_init(void)
{
    audio_mixer_reset();
    g_head = 0;
    g_count = 0;
    g_played = 0;
    g_captured = 0;
    g_dropped = 0;
    g_rate = AUDIO_RATE_DEFAULT;
    if (!usb_audio_ready()) {
        kprintf("CRUGOS: audio device absent\n");
        return -1;
    }
    if (g_rate_requested)
        g_rate = g_rate_requested;
    else if (usb_audio_rate())
        g_rate = usb_audio_rate();
    audio_mixer_set_volume(0, g_volume);
    audio_mixer_set_volume(1, g_volume);
    kprintf("CRUGOS: audio device ready (usb %s, %u ch, %u-bit, %u Hz)\n",
            usb_audio_class_name(), (unsigned)usb_audio_channels(),
            (unsigned)usb_audio_bits(), (unsigned)g_rate);
    return 0;
}

int audio_ready(void)
{
    return usb_audio_ready();
}

u32 audio_rate(void)
{
    return g_rate;
}

u32 audio_channels(void)
{
    return AUDIO_CHANNELS;
}

u32 audio_write(const i16 *frames, u32 count)
{
    u32 space, n, i, w;

    if (!frames || count == 0)
        return 0;
    space = AUDIO_RING_FRAMES - g_count;
    n = count < space ? count : space;
    w = (g_head + g_count) % AUDIO_RING_FRAMES;
    for (i = 0; i < n; i++) {
        g_ring[w * 2u] = frames[i * 2u];
        g_ring[w * 2u + 1u] = frames[i * 2u + 1u];
        w = (w + 1u) % AUDIO_RING_FRAMES;
    }
    g_count += n;
    g_dropped += count - n;
    return n;
}

u32 audio_read(i16 *frames, u32 count)
{
    u32 n, i;

    if (!frames || count == 0)
        return 0;
    n = count < g_count ? count : g_count;
    for (i = 0; i < n; i++) {
        frames[i * 2u] = g_ring[g_head * 2u];
        frames[i * 2u + 1u] = g_ring[g_head * 2u + 1u];
        g_head = (g_head + 1u) % AUDIO_RING_FRAMES;
    }
    g_count -= n;
    audio_mixer_apply(frames, n);
    for (i = n; i < count; i++) {
        frames[i * 2u] = 0;
        frames[i * 2u + 1u] = 0;
    }
    return n;
}

void audio_played(u32 frames)
{
    g_played += frames;
}

void audio_captured(u32 frames)
{
    g_captured += frames;
}

u64 audio_frames_played(void)
{
    return g_played;
}

u64 audio_frames_captured(void)
{
    return g_captured;
}

u32 audio_dropped_frames(void)
{
    return g_dropped;
}

int audio_tone(i16 *out, u32 frames, u32 *phase, u32 rate, u32 freq)
{
    u32 step, i, ph;

    if (!out || !phase || rate == 0)
        return -1;
    if (freq > rate / 2u)
        freq = rate / 2u;
    step = (u32)(((u64)freq * 360u * 65536u) / rate);
    ph = *phase;
    for (i = 0; i < frames; i++) {
        u32 idx = (ph >> 16) % 360u;
        i16 sample = g_sine[idx];

        out[i * 2u] = sample;
        out[i * 2u + 1u] = sample;
        ph += step;
        if (ph >= (360u * 65536u))
            ph -= 360u * 65536u;
    }
    *phase = ph;
    return 0;
}

int audio_selftest(void)
{
    static i16 chunk[AUDIO_PACKET_FRAMES * AUDIO_CHANNELS];
    u32 target, written = 0, phase = 0, elapsed_ms;
    u64 start, deadline;

    /* audio_init() already reported the absent device; stay quiet here. */
    if (!audio_ready())
        return 1;
    if (usb_audio_set_volume(g_volume) != 0)
        kprintf("CRUGOS: audio volume unsupported\n");
    kprintf("CRUGOS: audio volume (ch0 %d/256 dB, ch1 %d/256 dB, mute %u)\n",
            (int)audio_mixer_volume(0), (int)audio_mixer_volume(1),
            (unsigned)audio_mixer_mute(0));
    if (usb_audio_start(g_rate, AUDIO_CHANNELS, AUDIO_BITS_DEFAULT) != 0) {
        kprintf("CRUGOS: audio stream failed\n");
        return 0;
    }
    kprintf("CRUGOS: audio stream started (%u Hz, %u ch)\n",
            (unsigned)g_rate, (unsigned)AUDIO_CHANNELS);

    target = g_seconds ? g_seconds * g_rate : (g_rate / 2u);
    start = timer_monotonic_ns();
    deadline = start + (u64)AUDIO_SELFTEST_MS * 1000000ull;
    while (written < target && timer_monotonic_ns() < deadline) {
        u32 n = target - written;
        u32 accepted;

        if (n > AUDIO_PACKET_FRAMES)
            n = AUDIO_PACKET_FRAMES;
        if (audio_tone(chunk, n, &phase, g_rate, AUDIO_SELFTEST_FREQ) != 0)
            break;
        accepted = audio_write(chunk, n);
        written += accepted;
        usb_poll();
        if (accepted == 0)
            cpu_wait_for_irq();
    }
    while (audio_frames_played() < target && timer_monotonic_ns() < deadline) {
        usb_poll();
        if (audio_frames_played() < target)
            cpu_wait_for_irq();
    }
    elapsed_ms = (u32)((timer_monotonic_ns() - start) / 1000000ull);
    if (audio_frames_played() < target) {
        kprintf("CRUGOS: audio stream timeout\n");
        usb_audio_stop();
        return 0;
    }
    kprintf("CRUGOS: audio streamed %u frame(s) in %u ms\n",
            (unsigned)audio_frames_played(), (unsigned)elapsed_ms);
    if (elapsed_ms < 100u) {
        kprintf("CRUGOS: audio stream timeout (not paced)\n");
        usb_audio_stop();
        return 0;
    }

    if (usb_audio_capture_available()) {
        u32 before = (u32)audio_frames_captured();
        u64 cap_end = timer_monotonic_ns() + (u64)AUDIO_CAPTURE_MS * 1000000ull;

        if (usb_audio_capture_start() == 0) {
            while (timer_monotonic_ns() < cap_end) {
                usb_poll();
                cpu_wait_for_irq();
            }
        }
        kprintf("CRUGOS: audio captured %u frame(s)\n",
                (unsigned)(audio_frames_captured() - before));
    } else {
        kprintf("CRUGOS: audio capture unavailable (no input endpoint)\n");
    }
    usb_audio_stop();
    kprintf("CRUGOS: audio selftest ok\n");
    return 1;
}
