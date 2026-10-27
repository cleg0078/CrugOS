# Console input: PS/2 keyboard and input.device

CrugOS can now take keyboard input while it boots. Phase 4A adds a polled
i8042/PS-2 keyboard driver, a scancode decoder that feeds a small byte queue
(`input.device`), and a console read path that merges the serial line and the
keyboard. The first consumer is the encrypted-root passphrase prompt from phase
3B-10: until now it could only be answered over the serial line.

```
i8042 controller (0x60 data, 0x64 status)   COM1 (0x3F8)
        |                                        |
        v                                        v
keyboard_poll() -> input_scancode()          serial_try_getc()
        |                                        |
        v                                        |
byte queue (64 B) -> input_try_getc()            |
        +---------------- console_try_getc() ----+
                              |
                     storage_prompt_line() -> lineedit -> crypt_unlock()
```

## Why PS/2 first

The spec's input roadmap is USB HID over xHCI (phase 5). PS/2 is the smaller
step that still proves the whole path end to end: a real controller, a real
scancode stream, a device-independent byte queue, and a consumer. It also makes
the encrypted boot testable headlessly: QEMU injects keys into the emulated
i8042 through QMP, so a gate can unlock the root with a keyboard without any
display or physical hardware.

## i8042 driver (`kernel/arch/x86_64/keyboard.c`)

`keyboard_init()` programs the controller without interrupts and returns 0 when
a keyboard is present:

1. read the status port; `0xFF` means no controller at all (`-1`);
2. `0xAD` disables the first PS/2 port, then drain the output buffer;
3. `0x20` reads the configuration byte: set bit 6 (translate to set 1), clear
   bits 0-1 (no IRQ1/IRQ12), clear bit 4 (clock enabled);
4. `0x60` writes the configuration byte back, `0xAE` re-enables the port;
5. `0xF4` enables scanning and swallows the `0xFA` ACK.

`keyboard_poll()` runs from the console read path and is limited to 64
iterations per call so a stuck controller cannot spin forever. Bytes with the
AUX bit (status bit 5) set belong to the mouse and are discarded.

The translation bit matters: the controller converts the keyboard's set-2
scancodes into set 1, which is what the decoder below expects.

## Scancode decoder (`kernel/input/input.c`)

The decoder is freestanding and has no hardware or kernel dependencies, so the
host test binary links it directly.

- Make/break is the high bit: `scan & 0x80` means release. Characters are only
  produced on make; modifier keys update on both make and break.
- `0xE0` sets an extended prefix. `E0 1D` / `E0 38` update right Ctrl and right
  Alt; other extended keys (arrows, Home/End, Insert/Delete) are ignored for
  now. `0xE1` (Pause) is ignored.
- `0xFA` (ACK) and `0xEE` (echo) are ignored.
- Modifiers: `0x2A`/`0x36` Shift, `0x1D` Ctrl, `0x38` Alt, `0x3A` Caps Lock
  (toggles on make only). Caps applies only to letter scan codes and combines
  with Shift as XOR.
- Named keys: `0x1C` Enter -> `\n`, `0x0E` Backspace -> `0x7F`, `0x0F` Tab ->
  `\t`, `0x01` Escape -> `0x1B`.
- Ctrl chords translate to control bytes: letters to `0x01`..`0x1A` (`Ctrl+C`
  is `0x03`), `[`..`_` to `0x1B`..`0x1F`; `Ctrl+Space` and `Ctrl+@` produce
  nothing.
- `g_unshifted[0x59]` / `g_shifted[0x59]` hold the US layout rows (digits,
  letters, `-`, `=`, `[`, `]`, `;`, `'`, `` ` ``, `\`, `,`, `.`, `/`, Space).
  Unmapped codes produce nothing.

`input_try_getc()` returns the next byte or `-1` when the queue is empty. The
queue holds 64 bytes; further keys are dropped until the consumer drains it.

## Console merge (`kernel/core/console.c`)

`console_try_getc()` is the single read path for console input:

```c
int console_try_getc(void)
{
    int c = serial_try_getc();

    if (c >= 0)
        return c;
    keyboard_poll();
    return input_try_getc();
}
```

Serial is drained first so an automation harness that pipes `crypt.pass` over
COM1 keeps working; the keyboard is polled only when the serial line is idle.
Output is unchanged: every byte still goes to both the serial port and the
framebuffer.

## Boot integration

`input_init()` and `keyboard_init()` run right after the heap is ready, before
the storage stack, because the storage passphrase prompt needs input:

```
CRUGOS: heap ready
CRUGOS: input ready (serial + keyboard)
```

`CRUGOS: input ready (serial only)` is printed when no i8042 keyboard was found
(for example under `-machine` variants without PS/2); the kernel then behaves
exactly like phase 3B-10. The passphrase prompt itself is unchanged apart from
its wording:

```
CRUGOS: crypt passphrase required
CRUGOS: type the passphrase on the console (serial or keyboard) (3 attempt(s))
passphrase: ***********
```

## Host tests

`tests/host/test_input.c` links `kernel/input/input.c` into the host unit-test
binary and covers the decode matrix: letters and Shift, Caps XOR Shift,
digits and shifted symbols, punctuation, Enter/Tab/Backspace/Escape/Space,
`Ctrl+C` and its release, `Ctrl+[`, ignored `E0` arrows, right Ctrl, ACK/echo,
release-only and unmapped codes, and queue overflow (100 keys yield exactly 64
bytes). `make test-host` reports 903 checks including these.

## Gates

| Target | What it proves |
| --- | --- |
| `make test` | the base boot still comes up and prints `CRUGOS: input ready (serial + keyboard)` |
| `make test-crypt-keyboard` | a headless QMP `send-key` stream types the passphrase into the emulated i8042 and the encrypted root mounts |
| `make test-crypt-keyboard-wrong` | three wrong keyboard passphrases are rejected, the volume stays locked, and boot fails closed |

`scripts/run-crypt-keyboard.py` drives the keyboard gates. It boots QEMU with
`CRYPT_PASS=` (so no `crypt.pass=` token is written), adds
`-qmp unix:<tmp>/qmp.sock,server=on,wait=off`, waits for `passphrase: ` in the
serial log, and sends one `send-key` per character with a 40 ms hold time and a
40 ms gap. The serial line stays a log file, so the kernel falls through to the
keyboard. The script asserts the passphrase never appears in the serial log, the
prompt count, the ready/locked marker, and the QEMU exit status (33 unlocked,
35 locked).

## Typing a passphrase by hand

The gates above are headless. To try the prompt yourself, boot an encrypted
disk in a QEMU window and type on the host keyboard - QEMU forwards it to the
emulated PS/2 controller, so the kernel sees exactly the same scancodes as on
real hardware:

```
make crypt-run                              # gtk window, 10 minute timeout
make crypt-run CRYPT_RUN_DISPLAY=sdl        # another display backend
make crypt-run CRYPT_RUN_TIMEOUT_SEC=120    # shorter window
```

`crypt-run` rebuilds `$(CRYPT_IMG)` and therefore reseals the passphrases, then
boots it with `CRYPT_PASS=` so the kernel must prompt. Type `crugos-test` and
press Enter (the recovery slot's `crugos-recovery` also unlocks it). Seal a
different passphrase with `make crypt-run CRYPT_PASS=my-passphrase`, then type
that one. `Ctrl+Alt+G` releases the mouse/keyboard grab; closing the window ends
the run. The serial log (`build/<arch>/serial.log`) records the masked prompt and
never the passphrase. `make run ... QEMU_DISPLAY=none` still works for headless
boots, but then only a harness can answer the prompt.

On real hardware, copy `build/<arch>/esp` onto a FAT32 EFI system partition and
the encrypted image onto a spare SATA/NVMe disk (the kernel never touches the ESP
disk as a root candidate). Boot UEFI-only with Secure Boot off. A PS/2 keyboard
works directly; a USB keyboard only works through the firmware's legacy USB
emulation, because there is no xHCI/HID driver yet. Consumer boards rarely
expose a COM1 header, so watch the framebuffer console for the prompt.

## Limits

- Polled only: no IRQ1 handler, no key repeat, no LED state, no Num Lock.
- US layout only, one keyboard, no remapping.
- Extended keys are decoded as modifiers only; arrow keys and editing keys are
  not delivered yet.
- No USB HID keyboard, no framebuffer/GOP input, no VT switching (phase 5 work).
- The keyboard gate types at 40 ms per key, so its encrypted boot takes about a
  second longer than the serial prompt gate; Argon2id still dominates the
  non-typing cost.
