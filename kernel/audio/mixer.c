/* Audio mixer: per-channel volume (1/256 dB) and mute (phase 5C). */
#include <crug/types.h>
#include <crug/kernel.h>
#include <crug/audio.h>

/* 10^(-db/20) in Q15 for 0 dB down to -30 dB in 1 dB steps. */
static const u32 g_gain_db[31] = {
    32768, 29205, 26029, 23198, 20675, 18427, 16423, 14637,
    13045, 11627, 10362,  9235,  8231,  7336,  6538,  5827,
     5193,  4629,  4125,  3677,  3277,  2920,  2603,  2320,
     2068,  1843,  1642,  1464,  1305,  1163,  1036
};

static i32 g_volume[AUDIO_CHANNELS];
static u32 g_mute[AUDIO_CHANNELS];

static u32 clamp_volume(i32 volume)
{
    if (volume > AUDIO_VOLUME_MAX)
        volume = AUDIO_VOLUME_MAX;
    if (volume < AUDIO_VOLUME_MIN)
        volume = AUDIO_VOLUME_MIN;
    return (u32)(-volume);
}

void audio_mixer_reset(void)
{
    u32 ch;

    for (ch = 0; ch < AUDIO_CHANNELS; ch++) {
        g_volume[ch] = 0;
        g_mute[ch] = 0;
    }
}

int audio_mixer_set_volume(u32 channel, i32 volume)
{
    if (channel >= AUDIO_CHANNELS)
        return -1;
    (void)clamp_volume(volume);
    if (volume > AUDIO_VOLUME_MAX)
        volume = AUDIO_VOLUME_MAX;
    if (volume < AUDIO_VOLUME_MIN)
        volume = AUDIO_VOLUME_MIN;
    g_volume[channel] = volume;
    return 0;
}

int audio_mixer_set_mute(u32 channel, u32 mute)
{
    if (channel >= AUDIO_CHANNELS)
        return -1;
    g_mute[channel] = mute ? 1u : 0u;
    return 0;
}

i32 audio_mixer_volume(u32 channel)
{
    if (channel >= AUDIO_CHANNELS)
        return 0;
    return g_volume[channel];
}

u32 audio_mixer_mute(u32 channel)
{
    if (channel >= AUDIO_CHANNELS)
        return 0;
    return g_mute[channel];
}

u32 audio_mixer_gain(u32 channel)
{
    u32 attenuation;
    u32 db;

    if (channel >= AUDIO_CHANNELS || g_mute[channel])
        return 0;
    attenuation = clamp_volume(g_volume[channel]);
    if (attenuation == 0)
        return 32768u;
    /* Round to the nearest whole decibel of the table. */
    db = (attenuation + 128u) / 256u;
    if (db > 30u)
        db = 30u;
    return g_gain_db[db];
}

void audio_mixer_apply(i16 *frames, u32 count)
{
    u32 gl = audio_mixer_gain(0);
    u32 gr = audio_mixer_gain(1);
    u32 i;

    if (!frames || count == 0)
        return;
    if (gl == 32768u && gr == 32768u)
        return;
    for (i = 0; i < count; i++) {
        i32 l = (i32)frames[i * 2u];
        i32 r = (i32)frames[i * 2u + 1u];

        frames[i * 2u] = (i16)((l * (i32)gl) >> 15);
        frames[i * 2u + 1u] = (i16)((r * (i32)gr) >> 15);
    }
}
