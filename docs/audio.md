# Audio: audio.device, the mixer and USB Audio Class

Phase 5C adds the first audio path to the kernel: a 16-bit stereo PCM ring
(`audio.device`), a per-channel volume mixer and a USB Audio Class driver that
parses the configuration descriptor of an audio function, opens its
isochronous endpoint and feeds it from the ring.

The kernel has no idle thread yet, so the stream is driven by the boot
self-test and by `usb_poll()` -- the same polled model as the HID keyboard and
mouse from phases 5A and 5B. There are no interrupts, no DMA callback and no
audio thread: a poll drains the xHCI event ring, and every isochronous
completion refills the packet it just retired.

```
  audio.device                     mixer                     usb audio
  +----------------------+   +------------------+   +------------------------+
  | ring 16384 frames    |   | ch0 gain/mute    |   | uac1/uac2 descriptor   |
  | interleaved i16 L/R  |-->| ch1 gain/mute    |-->| 32 isoc OUT packets    |
  | write/read + counters|   | Q15 0..-30 dB    |   | 48 frames per packet   |
  +----------------------+   +------------------+   +-----------+------------+
           ^                                                  |
           |  audio_played()/audio_captured()                  v
           +-------------------------------------------- USB isochronous
                                                        (xHCI, polled)
```

## audio.device

`include/crug/audio.h` fixes the format the kernel speaks everywhere:
interleaved signed 16-bit stereo, 48000 Hz by default.

| Constant | Value | Meaning |
| --- | --- | --- |
| `AUDIO_CHANNELS` | 2 | left and right |
| `AUDIO_RATE_DEFAULT` | 48000 | frames per second |
| `AUDIO_BITS_DEFAULT` | 16 | bits per sample |
| `AUDIO_FRAME_BYTES` | 4 | one frame is L and R |
| `AUDIO_RING_FRAMES` | 16384 | 64 KiB, 341 ms at 48 kHz |
| `AUDIO_PACKET_FRAMES` | 48 | one 1 ms USB packet at 48 kHz |

The ring lives in `kernel/audio/audio.c` as one static `i16` array. It is a
single producer/single consumer ring: `audio_write()` appends and returns how
many frames it accepted, `audio_read()` pops and returns how many it delivered.
Overflow is never silent -- the frames that did not fit are counted and reported
by `audio_dropped_frames()`.

`audio_read()` applies the mixer to the frames it just popped and then
zero-fills the rest of the caller's buffer, so a short read still hands the
hardware a complete packet of silence instead of stale samples.

Counters: `audio_played()` and `audio_captured()` are called by the USB driver
with the number of frames the hardware retired (playback) or filled (capture);
`audio_frames_played()` and `audio_frames_captured()` expose the totals to the
self-test and to any future player.

`audio_tone()` generates a test tone without any table per frequency. It uses a
360-entry Q15 sine table and a phase accumulator whose step is
`(freq * 360 * 65536) / rate`; at 158400 Hz the step is exactly one table entry,
which is what the host tests rely on. Frequencies above Nyquist are clamped to
`rate / 2`.

## Mixer

`kernel/audio/mixer.c` keeps one volume and one mute flag per channel. Volume is
in 1/256 dB -- the unit the USB Audio Class uses -- and is clamped to
`AUDIO_VOLUME_MIN` (-7680, i.e. -30 dB) through `AUDIO_VOLUME_MAX` (0 dB).

`audio_mixer_gain()` converts that to a Q15 multiplier through a 31-entry table
for 0 dB down to -30 dB in whole steps:

```
32768, 29205, 26029, 23198, 20675, 18427, 16423, 14637, 13045, 11627, 10362,
 9235,  8231,  7336,  6538,  5827,  5193,  4629,  4125,  3677,  3277,  2920,
 2603,  2320,  2068,  1843,  1642,  1464,  1305,  1163,  1036
```

Attenuation rounds to the nearest whole dB (`(attenuation + 128) / 256`), so
-1.5 dB becomes -2 dB rather than -1 dB. Mute beats volume and yields a gain of
zero. `audio_mixer_apply()` returns immediately when both channels are at unity,
otherwise it multiplies each sample with `(sample * gain) >> 15`.

The default volume is -2560 (-10 dB), which is quiet enough that a boot tone is
not startling but still audible; `audio.volume=` overrides it.

## USB Audio Class driver

`drivers/usb/usb_audio.c` is attached by the xHCI driver: when
`xhci_enumerate()` reads the configuration descriptor and interface 0 has class
`0x01` (audio), it calls

```c
void usb_audio_attach(u32 slot, u32 port, u32 speed,
                      const u8 *config, u32 config_len);
```

The driver parses the descriptor *before* any control transfer, because the
xHCI driver reuses its 512-byte control buffer for every request.

### Descriptor parsing

Both class versions are understood, and the descriptor is walked as a chain of
`bLength`/`bDescriptorType` records:

* **UAC1** (audio class header `bcdADC` 0x0100): input terminal (streaming,
  0x0101), feature unit with per-channel controls, output terminal (speaker),
  an audio streaming interface with alternate setting 0 (no bandwidth) and 1
  (one isochronous endpoint), the AS general descriptor carrying
  `bTerminalLink` and a format type I descriptor with the channel count,
  subframe size, bit resolution and sample rate.
* **UAC2** (header `bcdADC` 0x0200): the same shape with a clock source entity
  and 4-byte sample-rate controls on the streaming side.

From that the driver records the card: class version, channel count, bit depth,
sample rate, the streaming interface number, the alternate setting, the
isochronous endpoint address and packet size, and whether a capture (IN)
endpoint exists at all.

### Bring-up

1. `SET_INTERFACE` selects alternate setting 1 on the streaming interface, which
   is what makes the endpoint consume bandwidth at all. Failure is fatal:
   `CRUGOS: usb audio alt setting failed`.
2. The sampling frequency is selected with `SET_CUR`: UAC1 sends a 3-byte rate
   to the endpoint, UAC2 a 4-byte rate to the clock source entity. A failure is
   non-fatal (`CRUGOS: usb audio rate request failed`) because devices are
   allowed to accept only the rate they advertise, which is exactly what we
   asked for.
3. The feature unit volume is written per channel with a 2-byte little-endian
   signed 1/256 dB value, and mute with one byte. Both are non-fatal;
   `audio.device` prints `CRUGOS: audio volume unsupported` and keeps the
   software mixer as the single source of truth.

The control transfers go through `usb_control()` in `drivers/usb/xhci.c`, which
queues a three-stage control TD (setup, optional data, status) and waits for its
transfer event.

QEMU's `usb-audio` has a quirk worth knowing: its volume handler computes the
channel as `wValue - 1`, so every volume `SET_CUR` with channel selector 0x02
addresses an out-of-range channel and the request stalls. Mute works. The kernel
therefore treats a failing volume request as expected and continues.

### Streaming

The playback path keeps 32 packets in flight, one per millisecond at 48 kHz:

* `usb_isoc_open()` allocates a ring of 32 packet buffers of `wMaxPacketSize`
  bytes (192 bytes, i.e. 48 frames, for QEMU) and configures the endpoint.
* `uac_fill_out()` tops the window up from `audio_read()`: it compares the
  packets *submitted* against the packets *completed successfully* and submits
  until the window is full.
* Each packet is one normal TRB with the interrupt-on-completion flag and
  `TRB_SIA` set.
* Every completion retires a packet; `usb_audio_poll()` turns the delta of
  completed packets into `audio_played()` frames and refills the window.

Only success and short-packet completions consume a packet. Ring underruns
(`CC 14`) are counted as errors for diagnostics but do not count as completed
packets -- counting them made the in-flight window wrap and stalled the stream
after the first 32 packets, which is the bug the phase 5C bring-up hit.

Capture is the mirror image: the driver submits IN packets, and each completion
pushes the filled frames through `audio_captured()`. A device without an IN
endpoint reports `CRUGOS: audio capture unavailable (no input endpoint)`.

### Isochronous support in xHCI

`drivers/usb/xhci.c` gained a small isochronous API used only by the audio
driver:

| Function | Purpose |
| --- | --- |
| `usb_control()` | one synchronous control transfer |
| `usb_isoc_open()` | allocate packet buffers and configure the endpoint |
| `usb_isoc_submit()` | queue one packet |
| `usb_isoc_buf()` / `usb_isoc_buf_pa()` | packet buffer and its physical address |
| `usb_isoc_packets()` | packets in the ring (32) |
| `usb_isoc_done()` / `usb_isoc_errors()` | counters |
| `usb_isoc_close()` | release the endpoint |

Two details decide whether a stream moves at all:

* **Interval encoding.** An endpoint context stores the interval as a power of
  two in microframes. A full- or low-speed endpoint reports its interval in
  frames, and one frame is eight microframes, so it needs two extra bits
  (`interval + 2`); a high- or super-speed endpoint already reports
  microframes (`interval - 1`). QEMU's audio endpoint reports `bInterval` 1 at
  full speed, which becomes context interval 3 -- 8 microframes, exactly 1 ms.
* **`TRB_SIA`.** Without the "schedule as soon as possible" bit, QEMU parks
  every packet at frame ID 0 of its 16384-microframe (2.048 s) window, so the
  first packet completes and the rest appear to hang. With `TRB_SIA` the
  controller picks the next real frame.

## Command line

`audio_cmdline()` reads whitespace-separated tokens from the boot command line.
Malformed tokens are ignored, and so are values outside the ranges below.

| Token | Range | Meaning |
| --- | --- | --- |
| `audio.rate=<hz>` | 8000..192000 | request this sample rate |
| `audio.volume=<1/256 dB>` | -7680..0 | initial mixer volume, default -2560 |
| `audio.seconds=<n>` | 0..5 | stream for `n` seconds in the self-test (default 0.5) |

## Boot integration

`usb_init()` enumerates the audio function during controller bring-up, so the
card exists before `audio_init()` runs. `audio_init()` resets the mixer, picks
the rate (command line, else the device's, else 48000), applies the volume to
both channels and prints

```
CRUGOS: usb audio attached (uac1, 2 ch, 16-bit, 48000 Hz, iface 1 alt 1)
CRUGOS: audio device ready (usb uac1, 2 ch, 16-bit, 48000 Hz)
```

The self-test runs after the USB self-test and is where the audio path is
actually exercised:

```
CRUGOS: audio volume unsupported
CRUGOS: audio volume (ch0 -2560/256 dB, ch1 -2560/256 dB, mute 0)
CRUGOS: audio stream started (48000 Hz, 2 ch)
CRUGOS: audio streamed 24144 frame(s) in 498 ms
CRUGOS: audio capture unavailable (no input endpoint)
CRUGOS: audio selftest ok
```

Failures are reported as

* `CRUGOS: audio device absent` (returned as success -- a machine without an
  audio function is not a boot failure),
* `CRUGOS: audio stream failed`,
* `CRUGOS: audio stream timeout`, or `… (not paced)` when the frames were
  delivered faster than real time,
* `CRUGOS: selftest: audio` panic from `selftest.c`.

## Host tests

`tests/host/test_audio.c` links `kernel/audio/audio.c` and
`kernel/audio/mixer.c` against a fake USB Audio Class device and covers:

* the mixer: unity passthrough, the table at -1 dB and -10 dB, rounding to whole
  dB, clamping at both ends, mute beating volume, out-of-range channels and
  empty buffers;
* the tone generator: one exact period from the phase step, silence at 0 Hz,
  Nyquist clamping and invalid arguments;
* the ring: round trip, zero-filled short reads, overflow accounting, reads
  passing through the mixer, counters and null arguments;
* the command line and init: rate/volume/seconds parsing, rejected tokens, and a
  device-less init that fails without failing the self-test.

`make test-host` reports the total (2372 checks at phase 5C).

## Gates

`make test-audio` boots with a QEMU audio function attached to a QEMU xHCI
controller and no other keyboard or audio device:

```
-device qemu-xhci,id=xhci -device usb-audio,audiodev=audio0 -audiodev none,id=audio0
```

`-audiodev none` keeps the gate hermetic: no host sound server is contacted, so
the gate works on a machine with no `/dev/snd` at all. `scripts/boot-check.py`
then requires `EXPECT_AUDIO=yes`, which means the attach marker, the device
ready marker, the stream-start marker, a `streamed N frame(s) in M ms` line and a
capture line (or the "capture unavailable" marker), plus the `phase 5c` marker
and QEMU exit status 33. `scripts/check-disk.py` still runs, because the disk is
written during the same boot.

## Limits

* Polled only. Streaming advances on `usb_poll()` at the 10 ms tick, which is
  fine for a 32 ms packet window but is not a real-time audio path.
* 16-bit stereo 48 kHz only; UAC2 clock selectors, multiple sample rates per
  format and other bit depths are parsed but not negotiated.
* One audio function at a time, no hubs, no device-side effects units, no
  MIDI.
* Capture is implemented but could not be gated end to end here: QEMU's
  `usb-audio` exposes only the playback interface on this host, so the gate
  accepts the "capture unavailable" marker.
* No resampling: a device that only offers e.g. 44100 Hz is driven at 44100 Hz
  while the ring keeps whatever rate the caller assumed.
