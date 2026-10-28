#ifndef CRUG_AUDIO_H
#define CRUG_AUDIO_H

#include <crug/types.h>
#include <crug/bootinfo.h>

/* audio.device, the mixer and the USB Audio Class driver (phase 5C).

   The device is a 16-bit stereo PCM ring. The mixer applies a per-channel
   volume to everything that leaves the ring for the hardware, and the USB
   audio driver (drivers/usb/usb_audio.c) drains the ring into isochronous
   packets. There is no idle thread yet, so streaming is driven by the boot
   self-test and by usb_poll(). */

#define AUDIO_CHANNELS      2u
#define AUDIO_RATE_DEFAULT  48000u
#define AUDIO_BITS_DEFAULT  16u
#define AUDIO_FRAME_BYTES   (AUDIO_CHANNELS * (AUDIO_BITS_DEFAULT / 8u))
#define AUDIO_RING_FRAMES   16384u     /* 341 ms at 48 kHz */
#define AUDIO_PACKET_FRAMES 48u        /* one 1 ms frame at 48 kHz */

/* Volume is expressed in 1/256 dB, the USB Audio unit. */
#define AUDIO_VOLUME_MIN    (-7680)    /* -30 dB */
#define AUDIO_VOLUME_MAX    0

struct audio_format {
    u32 rate;
    u16 channels;
    u16 bits;
};

/* Mixer (kernel/audio/mixer.c): one gain and one mute flag per channel. */
void audio_mixer_reset(void);
int audio_mixer_set_volume(u32 channel, i32 volume);
int audio_mixer_set_mute(u32 channel, u32 mute);
i32 audio_mixer_volume(u32 channel);
u32 audio_mixer_mute(u32 channel);
u32 audio_mixer_gain(u32 channel);
void audio_mixer_apply(i16 *frames, u32 count);

/* audio.device (kernel/audio/audio.c). */
int audio_init(void);
void audio_cmdline(const boot_info *bi);
int audio_ready(void);
u32 audio_rate(void);
u32 audio_channels(void);
u32 audio_write(const i16 *frames, u32 count);
u32 audio_read(i16 *frames, u32 count);
void audio_played(u32 frames);
void audio_captured(u32 frames);
u64 audio_frames_played(void);
u64 audio_frames_captured(void);
u32 audio_dropped_frames(void);
int audio_tone(i16 *out, u32 frames, u32 *phase, u32 rate, u32 freq);
int audio_selftest(void);

/* USB Audio Class driver (drivers/usb/usb_audio.c). The attach hook is called
   by the xHCI driver with the configuration descriptor; everything else is
   driven by audio.device and the boot self-test. */
int usb_audio_ready(void);
const char *usb_audio_class_name(void);
u32 usb_audio_rate(void);
u32 usb_audio_channels(void);
u32 usb_audio_bits(void);
int usb_audio_start(u32 rate, u32 channels, u32 bits);
void usb_audio_stop(void);
int usb_audio_set_volume(i32 volume);
int usb_audio_set_mute(u32 mute);
int usb_audio_capture_available(void);
int usb_audio_capture_start(void);

#endif /* CRUG_AUDIO_H */
