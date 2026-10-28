/* Host tests for audio.device and the mixer (kernel/audio/audio.c, mixer.c). */
#include <crug/audio.h>
#include <crug/bootinfo.h>
#include <crug/types.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

/* ---------------------------------------------------------------------- */
/* Fake USB Audio Class device behind audio.device.                       */
/* ---------------------------------------------------------------------- */

static int g_usb_ready = 1;
static const char *g_class_name = "uac1";
static u32 g_usb_rate = AUDIO_RATE_DEFAULT;
static int g_start_calls;
static int g_stop_calls;
static int g_capture_start_calls;
static int g_volume_result;
static i32 g_last_volume;
static u32 g_last_mute;
static int g_capture_available;

int usb_audio_ready(void)
{
    return g_usb_ready;
}

const char *usb_audio_class_name(void)
{
    return g_class_name ? g_class_name : "none";
}

u32 usb_audio_rate(void)
{
    return g_usb_rate;
}

u32 usb_audio_channels(void)
{
    return AUDIO_CHANNELS;
}

u32 usb_audio_bits(void)
{
    return AUDIO_BITS_DEFAULT;
}

int usb_audio_start(u32 rate, u32 channels, u32 bits)
{
    (void)rate;
    (void)channels;
    (void)bits;
    g_start_calls++;
    return 0;
}

void usb_audio_stop(void)
{
    g_stop_calls++;
}

int usb_audio_set_volume(i32 volume)
{
    g_last_volume = volume;
    return g_volume_result;
}

int usb_audio_set_mute(u32 mute)
{
    g_last_mute = mute;
    return 0;
}

int usb_audio_capture_available(void)
{
    return g_capture_available;
}

int usb_audio_capture_start(void)
{
    g_capture_start_calls++;
    return 0;
}

static void audio_reset_fakes(void)
{
    g_usb_ready = 1;
    g_class_name = "uac1";
    g_usb_rate = AUDIO_RATE_DEFAULT;
    g_start_calls = 0;
    g_stop_calls = 0;
    g_capture_start_calls = 0;
    g_volume_result = 0;
    g_last_volume = 0;
    g_last_mute = 0;
    g_capture_available = 0;
}

void usb_poll(void)
{
}

static void audio_cmd(const char *cmd)
{
    struct boot_info bi;

    if (!cmd) {
        audio_cmdline(NULL);
        return;
    }
    memset(&bi, 0, sizeof(bi));
    bi.command_line = cmd;
    bi.command_line_len = (u64)strlen(cmd);
    audio_cmdline(&bi);
}

/* ---------------------------------------------------------------------- */
/* Mixer                                                                  */
/* ---------------------------------------------------------------------- */

static void test_audio_mixer(void)
{
    i16 frames[2];

    audio_mixer_reset();
    CHECK(audio_mixer_volume(0) == 0);
    CHECK(audio_mixer_volume(1) == 0);
    CHECK(audio_mixer_mute(0) == 0);
    CHECK(audio_mixer_gain(0) == 32768u);
    CHECK(audio_mixer_gain(1) == 32768u);

    /* Unity gain must leave the frames untouched. */
    frames[0] = 16384;
    frames[1] = -16384;
    audio_mixer_apply(frames, 1);
    CHECK(frames[0] == 16384);
    CHECK(frames[1] == -16384);

    /* -1 dB and -10 dB come out of the Q15 table. */
    CHECK(audio_mixer_set_volume(0, -256) == 0);
    CHECK(audio_mixer_gain(0) == 29205u);
    CHECK(audio_mixer_set_volume(1, -2560) == 0);
    CHECK(audio_mixer_gain(1) == 10362u);

    /* Attenuation rounds to the nearest whole dB. */
    CHECK(audio_mixer_set_volume(0, -128) == 0);
    CHECK(audio_mixer_gain(0) == 29205u);
    CHECK(audio_mixer_set_volume(0, -384) == 0);
    CHECK(audio_mixer_gain(0) == 26029u);

    /* Volumes clamp to 0 dB and -30 dB. */
    CHECK(audio_mixer_set_volume(0, 100) == 0);
    CHECK(audio_mixer_volume(0) == 0);
    CHECK(audio_mixer_gain(0) == 32768u);
    CHECK(audio_mixer_set_volume(0, -99999) == 0);
    CHECK(audio_mixer_volume(0) == AUDIO_VOLUME_MIN);
    CHECK(audio_mixer_gain(0) == 1036u);

    /* Mute beats the volume and zeroes the channel. */
    CHECK(audio_mixer_set_volume(0, 0) == 0);
    CHECK(audio_mixer_set_volume(1, -2560) == 0);
    CHECK(audio_mixer_set_mute(0, 1) == 0);
    CHECK(audio_mixer_gain(0) == 0u);
    CHECK(audio_mixer_mute(1) == 0);
    frames[0] = 16384;
    frames[1] = 16384;
    audio_mixer_apply(frames, 1);
    CHECK(frames[0] == 0);
    CHECK(frames[1] == 5181);
    CHECK(audio_mixer_set_mute(0, 0) == 0);
    CHECK(audio_mixer_gain(0) == 32768u);

    /* Out-of-range channels are rejected without touching state. */
    CHECK(audio_mixer_set_volume(2, 0) == -1);
    CHECK(audio_mixer_set_mute(2, 1) == -1);
    CHECK(audio_mixer_volume(9) == 0);
    CHECK(audio_mixer_mute(9) == 0);
    CHECK(audio_mixer_gain(9) == 0u);

    /* Empty buffers are a no-op. */
    audio_mixer_apply(NULL, 4);
    audio_mixer_apply(frames, 0);
    audio_mixer_reset();
}

/* ---------------------------------------------------------------------- */
/* Tone generator                                                         */
/* ---------------------------------------------------------------------- */

static void test_audio_tone(void)
{
    i16 out[720];
    i16 clamped[720];
    u32 phase = 0;
    u32 phase2 = 0;
    u32 peak = 0;
    u32 i;

    /* rate / 360 == 440 makes the step exactly one table entry. */
    CHECK(audio_tone(out, 360, &phase, 158400u, 440u) == 0);
    CHECK(phase == 0);
    CHECK(out[0] == 0);
    CHECK(out[180 * 2] == 0);
    CHECK(out[90 * 2] == 32767);
    CHECK(out[90 * 2 + 1] == 32767);
    CHECK(out[270 * 2] == -32767);
    for (i = 0; i < 360; i++) {
        i32 sum;

        CHECK(out[i * 2] == out[i * 2 + 1]);
        CHECK(out[i * 2] > -32768 && out[i * 2] < 32768);
        sum = (i32)out[i * 2] + (i32)out[(i + 180) % 360 * 2];
        CHECK(sum >= -1 && sum <= 1);
        if ((u32)(out[i * 2] > 0 ? out[i * 2] : 0) > peak)
            peak = (u32)out[i * 2];
    }
    CHECK(peak == 32767u);

    /* A zero frequency is silence and does not move the phase. */
    CHECK(audio_tone(out, 8, &phase2, 48000u, 0u) == 0);
    for (i = 0; i < 8; i++) {
        CHECK(out[i * 2] == 0);
        CHECK(out[i * 2 + 1] == 0);
    }
    CHECK(phase2 == 0);

    /* Frequencies above Nyquist are clamped, so they match the clamp. */
    CHECK(audio_tone(out, 64, &phase, 48000u, 100000u) == 0);
    CHECK(audio_tone(clamped, 64, &phase2, 48000u, 24000u) == 0);
    CHECK(memcmp(out, clamped, 64 * sizeof(i16) * 2) == 0);

    /* Invalid inputs are rejected. */
    CHECK(audio_tone(NULL, 4, &phase, 48000u, 440u) == -1);
    CHECK(audio_tone(out, 4, NULL, 48000u, 440u) == -1);
    CHECK(audio_tone(out, 4, &phase, 0u, 440u) == -1);
}

/* ---------------------------------------------------------------------- */
/* Device ring                                                            */
/* ---------------------------------------------------------------------- */

static i16 g_big[AUDIO_RING_FRAMES * AUDIO_CHANNELS];

static void test_audio_ring(void)
{
    i16 frames[8];
    i16 out[8];
    u32 i;

    audio_reset_fakes();
    CHECK(audio_init() == 0);
    CHECK(audio_ready() == 1);
    CHECK(audio_rate() == AUDIO_RATE_DEFAULT);
    CHECK(audio_channels() == AUDIO_CHANNELS);
    CHECK(audio_frames_played() == 0);
    CHECK(audio_frames_captured() == 0);
    CHECK(audio_dropped_frames() == 0);

    /* audio_init() sets the mixer from audio.volume= (default -10 dB). */
    CHECK(audio_mixer_set_volume(0, 0) == 0);
    CHECK(audio_mixer_set_volume(1, 0) == 0);
    CHECK(audio_mixer_gain(0) == 32768u);

    for (i = 0; i < 4; i++) {
        frames[i * 2] = (i16)(100 * (i + 1));
        frames[i * 2 + 1] = (i16)(-100 * (i + 1));
    }
    CHECK(audio_write(frames, 4) == 4);
    CHECK(audio_read(out, 4) == 4);
    for (i = 0; i < 4; i++) {
        CHECK(out[i * 2] == frames[i * 2]);
        CHECK(out[i * 2 + 1] == frames[i * 2 + 1]);
    }

    /* An empty ring reads nothing and zero-fills the caller's buffer. */
    memset(out, 0x7f, sizeof(out));
    CHECK(audio_read(out, 4) == 0);
    for (i = 0; i < 8; i++)
        CHECK(out[i] == 0);

    /* Reads run through the mixer, so -10 dB on the left channel only. */
    CHECK(audio_mixer_set_volume(0, -2560) == 0);
    frames[0] = 16384;
    frames[1] = 16384;
    CHECK(audio_write(frames, 1) == 1);
    CHECK(audio_read(out, 1) == 1);
    CHECK(out[0] == 5181);
    CHECK(out[1] == 16384);
    CHECK(audio_mixer_set_volume(0, 0) == 0);

    /* The ring rejects the overflow and counts the dropped frames. */
    for (i = 0; i < AUDIO_RING_FRAMES * AUDIO_CHANNELS; i++)
        g_big[i] = (i16)(i & 0x3fff);
    CHECK(audio_write(g_big, AUDIO_RING_FRAMES + 10) == AUDIO_RING_FRAMES);
    CHECK(audio_dropped_frames() == 10);
    CHECK(audio_read(out, 1) == 1);
    CHECK(out[0] == g_big[0]);
    CHECK(out[1] == g_big[1]);

    CHECK(audio_write(NULL, 4) == 0);
    CHECK(audio_write(frames, 0) == 0);
    CHECK(audio_read(NULL, 4) == 0);
    CHECK(audio_read(out, 0) == 0);

    /* Counters are plain accumulators. */
    audio_played(48);
    audio_played(2);
    audio_captured(96);
    CHECK(audio_frames_played() == 50);
    CHECK(audio_frames_captured() == 96);
}

/* ---------------------------------------------------------------------- */
/* Command line and init                                                  */
/* ---------------------------------------------------------------------- */

static void test_audio_cmdline(void)
{
    audio_reset_fakes();

    /* `audio.rate=` wins over the device rate; volume and seconds parse. */
    audio_cmd("usb.wait=1 audio.rate=44100 audio.volume=-100000 audio.seconds=3");
    CHECK(audio_init() == 0);
    CHECK(audio_rate() == 44100);
    CHECK(audio_mixer_volume(0) == AUDIO_VOLUME_MIN);
    CHECK(audio_mixer_volume(1) == AUDIO_VOLUME_MIN);

    /* Unsupported or malformed tokens are ignored. */
    audio_cmd("audio.rate=abc audio.rate=100 audio.seconds=9 audio.volume=+-");
    CHECK(audio_init() == 0);
    CHECK(audio_rate() == 44100);
    audio_cmd("audio.volume=-256");
    CHECK(audio_init() == 0);
    CHECK(audio_mixer_volume(0) == -256);

    audio_cmd(NULL);
    audio_cmd("");

    /* Without a device init fails but the self-test still passes. */
    g_usb_ready = 0;
    CHECK(audio_init() == -1);
    CHECK(audio_ready() == 0);
    CHECK(audio_selftest() == 1);
    g_usb_ready = 1;
}

void test_audio_functions(void)
{
    printf("- mixer / tone / ring / cmdline\n");
    test_audio_mixer();
    test_audio_tone();
    test_audio_ring();
    test_audio_cmdline();
}
