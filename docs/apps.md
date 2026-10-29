# Application bundles

Phase 7A turns a user program into an object the desktop can show: an
`Applications` bundle under `/Applications`, a small `manifest.toml`
carrying the identity and the Amiga-style tool types, and a normal user ELF
payload inside it. The kernel reads the manifests once at boot, keeps a
bounded registry in static memory, and lets either the Workbench or the
ring-3 shell launch a bundle. `make test-app-bundles` is the 56th gate.

The format is deliberately tiny. There is no signing, no dependency
resolution and no per-application address space yet; what phase 7A proves is
that a program can arrive as data with metadata, be validated without a
heap, and be started by two independent front ends.

## Why bundles

Before phase 7A a program was only a path in `/bin`: `/bin/hello`,
`/bin/sh`, `/bin/fork`. Nothing in the system could answer "what programs
exist", "what are they called" or "what settings does this one take". The
Workbench needs exactly that list to draw a program as an icon with a window
of rows, and the shell needs it to offer `app list`.

A bundle keeps that knowledge next to the program instead of in the kernel.
Adding an application means writing one directory into the image:

- the program itself, built exactly like every other ring-3 program;
- a manifest naming it, versioning it and, optionally, carrying tool types
  (key/value settings in the Amiga tradition);
- an icon colour, which is all the desktop can render for an icon today.

The kernel side is a parser and a fixed table, so the manifest grammar is
the contract and the registry is just an array.

## The bundle layout

```
/Applications/
  Hello.app/
    manifest.toml
    bin/x86_64/app_hello
  Orbit.app/
    manifest.toml
    bin/x86_64/app_orbit
  Broken.app/
    manifest.toml            # deliberately invalid, used by the gate
    bin/x86_64/app_hello
```

Only two things are required for a bundle to be accepted:

1. `<bundle>/manifest.toml` must exist, be readable and be at most
   `APP_MANIFEST_MAX` (2048) bytes.
2. The manifest's `[exec] prog` must be a relative path inside the bundle
   whose target exists on the root filesystem.

Nothing else is inspected. The payload does not have to be named after the
bundle, the directory may contain anything else, and the program is a plain
user ELF: the same exec loader loads it, with the same W^X segment rules and
the same four-page user stack.

## The manifest

The manifest is a bounded TOML subset, not a general TOML implementation. It
has four sections and a fixed key set:

| Section | Key | Required | Meaning and limit |
| --- | --- | --- | --- |
| `[app]` | `id` | yes | lowercase letters, digits, `-`, `_`, `.`; at most 31 characters |
| `[app]` | `name` | yes | display name, printable ASCII without `"`; at most 31 characters |
| `[app]` | `version` | yes | exactly three numeric parts (`1.0.0`), at most 15 characters |
| `[app]` | `min_os` | no | three-part version compared against the kernel's own version |
| `[exec]` | `prog` | yes | relative path, no leading `/`, no `..`, at most 127 characters |
| `[tooltypes]` | `KEY = "value"` | no | at most 8 lines; key at most 15 characters, value at most 31 |
| `[icon]` | `rgb` | no | exactly six hex digits; defaults to `A0A0A0` |

The lexical rules are short enough to list:

- `#` starts a comment and runs to the end of the line; blank lines are
  allowed and ignored.
- Spaces around `=` and at both line edges are trimmed, so
  `id = "hello"` and `  id="hello"  ` mean the same thing.
- Section headers are `[app]`, `[exec]`, `[tooltypes]` and `[icon]`;
  any other section or key is `bad-entry`.
- Values must be double-quoted printable ASCII, no `"` inside.
- Key characters are letters in either case, digits, `_`, `-` and `.`.
  Tool-type keys are conventionally uppercase because Amiga tool types were,
  but lowercase is accepted.
- A repeated key inside a section is `dup-key`; a missing required key has
  its own reason (`no-id`, `no-name`, `no-version`, `no-prog`).
- `min_os` is a minimum, not an exact match: the bundle is refused only when
  the kernel version is older than the value.
- The whole file is bounded: a longer manifest is `manifest-too-large`
  before the parser even runs.

The manifest phase 7A installs for `Hello.app` is a complete example:

```toml
[app]
id = "hello"
name = "Hello"
version = "1.0.0"

[exec]
prog = "bin/x86_64/app_hello"

[tooltypes]
GREET = "world"

[icon]
rgb = "FFB000"
```

`Orbit.app` adds `MODE = "demo"` and the colour `30C0FF`; `Broken.app`
has no `version` key and is the bundle the gate expects the kernel to refuse.

## Rejection reasons

Every refusal prints one line, once, at boot and names a stable reason string:

```
CRUGOS: app rejected Broken.app (no-version)
```

The full set follows. Any of them costs only that bundle: the scan continues
and the boot is not failed.

| Reason | Meaning |
| --- | --- |
| `no-manifest` | `manifest.toml` is missing, unreadable or empty |
| `manifest-too-large` | the manifest is longer than 2048 bytes |
| `bad-entry` | unknown section or key, a line without `=`, text after a value, or a structurally broken line |
| `no-id` | `[app] id` is absent |
| `bad-id` | the id has an invalid character or does not fit 31 characters |
| `dup-key` | a key (including `id`, `name`, `version`, `min_os`, `prog`, `rgb`) appears twice in its section |
| `no-name` | `[app] name` is absent |
| `bad-name` | the name has a non-printable character or does not fit 31 characters |
| `no-version` | `[app] version` is absent |
| `bad-version` | the version is not exactly three numeric parts, or `min_os` is not |
| `min-os` | `min_os` is newer than this kernel |
| `no-prog` | `[exec] prog` is absent |
| `bad-prog` | the program path is empty, absolute, contains `..`, is too long, or its target does not exist |
| `bad-rgb` | the icon colour is not six hex digits |
| `too-many-tooltypes` | a ninth `KEY = "value"` line |
| `bad-tooltype` | a tool-type key over 15 or a value over 31 characters |
| `dup-id` | another accepted bundle already uses that id |
| `dup-name` | another accepted bundle already uses that name |
| `registry-full` | the registry already holds `APP_MAX` (8) applications |

Duplicate detection is by id first, then by name: the directory order on
disk does not decide which of two candidates wins, because the registry
inserts in id order and rejects the later one.

## The registry

`kernel/app/app.c` owns the registry; `kernel/app/manifest.c` is the pure
parser it calls. The parser has no `kprintf`, no framebuffer and no heap
access, which is why the whole grammar is host tested.

The registry is static:

```c
struct app g_apps[APP_MAX];   /* 8 entries, no allocation */
```

`app_init()` runs once during boot, after `storage_init()` and before
`sched_init()`, and does one pass:

1. reset the table and the rejected counter;
2. list `/Applications` into a 1024-byte buffer; entries not ending in
   `.app` are ignored, and a missing directory simply ends the scan;
3. for each `.app` directory, read `manifest.toml` into a 2049-byte buffer;
4. parse it, join `prog` onto the bundle directory and require the payload
   to exist through `ext4_lookup()`;
5. insert into `g_apps[]` in id order, refusing duplicates;
6. print `CRUGOS: app registry ready (N app(s), M rejected)`.

The last line prints on every boot, including a disk with no bundles:

```
CRUGOS: app registry ready (0 app(s), 0 rejected)
```

Metadata is read once. A bundle installed after boot is invisible to the
running system until the next reboot, and the gate rebuilds the disk image
rather than writing to the running one.

## Launching a bundle from the desktop

The Workbench gets the registry only when the boot token `wb.app=1` is set
(it also needs `wb.enable=1`). It then adds the `Applications:` icon,
drawn as a 64 by 48 drawer at screen `(16, 300)`, and prints:

```
CRUGOS: wb app icon ready (Applications:)
```

A double click (two presses on the icon within `WB_DOUBLE_CLICK_NS`, 600 ms)
opens the bundle window lazily, at `(560, 320)` with a `340` by `240`
body, titled `Applications:`:

```
CRUGOS: wb app ready (2 application(s))
```

The window lists one row per registry entry, 20 pixels apart, starting six
pixels below the body. The name sits at the left and the version at the
right; the selected row is filled with the amber highlight colour
`0x00FFB000` and drawn in the window colour. An empty registry draws
`no applications` instead.

Selection is two-step, like the rest of the Workbench:

- the first body press selects a row and prints
  `CRUGOS: wb app selected <id> (index i of N)`;
- a second press on the same row within 600 ms launches it.

Only one launch is in flight at a time. A press while a program is still
running prints `CRUGOS: wb app busy (tid N)` and does nothing; the desktop
keeps reaping the thread and prints its exit status:

```
CRUGOS: wb app launch hello tid 11
CRUGOS: wb app reaped tid 11 exit 0
```

A launch failure prints `CRUGOS: wb app launch <id> failed (<rc>)`. The
self-test waits for exactly `wb.acts` launches (default 1) and prints
`CRUGOS: wb app selftest ok (N click(s), M launch(es))`; if the program
never exits it gives up after `wb.wait` seconds with
`CRUGOS: wb app timeout (R/WANT)`.

## Launching a bundle from the shell

The shell has an `app` builtin with three subcommands:

| Command | Printed lines |
| --- | --- |
| `app list` | one `CRUGOS: sh app <id> <version>` per entry, then `CRUGOS: sh app count N` |
| `app info <id>` | `CRUGOS: sh app <id> <version>`, `... name <name>`, `... icon <RRGGBB>` |
| `app run <id>` | `CRUGOS: sh app <id> tid N`, waits, then `CRUGOS: sh app <id> exit N` |

Errors are equally explicit: `CRUGOS: sh error unknown app <id>`,
`CRUGOS: sh error cannot launch <id>` and
`CRUGOS: sh error app registry unavailable`; a bare `app` prints
`CRUGOS: sh app usage: app list|info <id>|run <id>`.

`app run` waits for the child because the shell is an ordinary user thread
with `SYS_WAIT` behind it; the exit code it prints is the child's. The gate
uses this path from `/etc/app.rc`, which is a script and nothing more:

```
app list
app info orbit
app run orbit
exit 9
```

## The syscall ABI

Three syscalls expose the registry; `SYS_MAX` moved to 28:

| Number | Name | Arguments | Returns |
| --- | --- | --- | --- |
| 25 | `SYS_APP_COUNT` | none | number of registered applications |
| 26 | `SYS_APP_INFO` | index, buffer, capacity | bytes written, or a negative error |
| 27 | `SYS_APP_SPAWN` | index, `u32 *tid` | the new thread id, or a negative error |

`SYS_APP_INFO` writes a single text record into a buffer of at most
`SYS_APP_INFO_MAX` (512) bytes:

```
<id>|<name>|<version>|<prog>|<RRGGBB>|<KEY=value;KEY=value>
```

There is no trailing newline, the colour is six uppercase hex digits, and
tool types are joined with `;` in manifest order. A bundle with no tool
types ends with the empty field after the final `|`:

```
hello|Hello|1.0.0|/Applications/Hello.app/bin/x86_64/app_hello|FFB000|GREET=world
```

The buffer is copied out through `uaccess` on the ring-3 path and through
the identity-map check on the kernel path, exactly like the other
string-returning syscalls. Errors are `SYS_ENOENT` for an index past the
end, `SYS_EFAULT` for a null or unmapped buffer, `SYS_EINVAL` when the
record would not fit, and on `SYS_APP_SPAWN` `SYS_EIO` when the exec
loader refuses the payload.

`SYS_APP_SPAWN` passes the caller's thread id as the child's parent, so the
same `SYS_WAIT` rules apply: the caller may wait for the program it started
and nothing else. The launched program receives `argc` 1 and `argv[0]`
set to its own bundle directory, which is how a program finds files that
live beside it. There is no second argument yet.

The user library wraps the three calls as `u_app_count()`,
`u_app_info(index, buf, len)` and `u_app_spawn(index)`; the shell and the
two gate payloads use only those.

## Markers

| Marker | Meaning |
| --- | --- |
| `CRUGOS: app registry ready (N app(s), M rejected)` | one line per boot, `0/0` without bundles |
| `CRUGOS: app rejected <dir> (<reason>)` | one line per refused bundle |
| `CRUGOS: wb app icon ready (Applications:)` | the desktop added the drawer icon |
| `CRUGOS: wb app ready (N application(s))` | the bundle window opened |
| `CRUGOS: wb app selected <id> (index i of N)` | a row was selected |
| `CRUGOS: wb app launch <id> tid N` | the desktop started a bundle |
| `CRUGOS: wb app reaped tid N exit E` | the desktop collected it |
| `CRUGOS: wb app busy (tid N)` | a second launch was refused while one runs |
| `CRUGOS: wb app selftest ok (N click(s), M launch(es))` | the desktop leg finished |
| `CRUGOS: wb app timeout (R/WANT)` | the desktop leg ran out of `wb.wait` |
| `CRUGOS: sh app count N` | the shell listed N applications |
| `CRUGOS: sh app <id> tid N` / `exit N` | the shell spawned and reaped a bundle |
| `CRUGOS: user app test start` / `ok (exit 9)` | the `user.test=7` leg ran `/etc/app.rc` |

The registry line and the rejection lines come from the kernel; the
`wb app` lines come from the desktop; the `sh` lines come from the
ring-3 shell. A disk without bundles prints the registry line with zeros and
nothing else from this table.

## The gate

`make test-app-bundles` is gate 56 and has two legs; both must pass.

**Leg one, the desktop.** The Makefile rebuilds the disk with the three
bundles, then `scripts/run-workbench-app.py` boots with
`wb.enable=1 wb.wait=30 wb.app=1 wb.acts=1`, hot-plugs a `usb-mouse` over
QMP (the machine has `usb=off,i8042=off`, so it is the only pointer), and
drives the screen:

1. wait for `CRUGOS: wb desktop ready`, the registry line, the mouse and the
   framebuffer cursor;
2. double click the icon centre at `(48, 324)` and wait for
   `CRUGOS: wb app ready (`;
3. take a screendump and prove the window body pixel `(700, 360)` is
   `(8, 16, 24)` and the title pixel `(600, 335)` is `(255, 176, 0)`;
4. double click the first row at `(600, 358)` and wait for the launch,
   the reap and the app self-test;
5. wait for the Workbench selftest, require guest exit 33.

`scripts/boot-check.py` then runs with the flag `--expect-app=yes` (a flag,
not a positional argument, so the positional order of the other gates is
unchanged). It requires the seven app markers and checks the registry line
has both counts non-zero, the window reports at least one application, and
the self-test counted at least one launch.

**Leg two, the shell.** A plain `make run` with
`CMDLINE_EXTRA="user.test=7"` boots the same disk, and the kernel runs
`/bin/sh /etc/app.rc`, requiring exit code 9. The boot-check for this leg
uses the ordinary user expectations (`EXPECT_USER` through
`EXPECT_FORKBIG`), and `CRUGOS: user app ok (exit 9)` is the marker that
the shell's `app list`, `app info` and `app run` all worked.

Both legs end with `scripts/check-disk.py` so a bundle launch that damaged
the filesystem would be seen.

| Variable | Default | Meaning |
| --- | --- | --- |
| `WB_APP_WAIT` | `30` | seconds the desktop self-test waits for its launches |
| `WB_APP_ACTS` | `1` | launches the desktop self-test waits for (the driver supports exactly 1) |
| `WB_APP_RUN_TIMEOUT_SEC` | `240` | wall clock budget of the desktop leg |
| `APP_RUN_TIMEOUT_SEC` | `300` | wall clock budget of the `user.test=7` leg |

The recorded output of the desktop leg reads:

```
wba: the Applications window opened
wba: body (700, 360) -> (8, 16, 24), title (600, 335) -> (255, 176, 0)
wba: the desktop launched a bundle
wba: the launched program exited
wba: the app selftest counted the launch
ok: app bundles and tool types (exit 33)
```

A failure is one of: the guest never reported the desktop or the registry
(a boot problem), the Applications window never opened (the icon double
click missed, or `wb.app=1` was not passed), the body or title pixel is
wrong (the window is not painted as expected), the launch or reap markers
are missing (the exec loader refused the payload or the desktop did not
collect it), or a timeout, which shows the last marker reached.

`make test-host` covers the parser and the helpers: valid `Hello`,
`Orbit` and `Broken` manifests, comments and blank lines, padded keys,
lowercase tool-type keys, a missing final newline, every rejection reason,
path joining, version comparison, the `SYS_APP_INFO` record and tool-type
lookup. It reports 8216 checks; `make size` reports a 296848 B (289 KiB)
kernel file and a 942394 B (920 KiB) load image. All 56 gates pass.

## Lessons

The manifest parser first sliced a key as everything before `=` without
trimming it. Every key therefore kept the padding in front of the `=`
(`id `, `prog `, `GREET `) and the character check rejected the space,
so every single manifest failed with `bad-entry` - a failure mode that
looked like a syntax problem in the manifests rather than a parser bug. The
fix trims trailing whitespace from the key before validation, and while
there it accepts uppercase key letters, since Amiga tool types are
traditionally written in capitals. The host tests now include padded keys
and lowercase tool-type keys for exactly that reason.

## Limits

- At most 8 applications (`APP_MAX`), and the registry itself is static, so
  the limit is a compile-time constant, not a budget.
- No signing and no permission checks: any bundle the kernel can parse is
  launchable by any user program. Capability checks are later work.
- The launch path runs one program with exactly one argument, its bundle
  directory. There is no second argument and no environment block yet.
- A rejected bundle is silently absent from the window and from
  `app list`; only the serial log names the reason.
- The icon is a colour plus a text label. There is no icon art, no
  per-bundle bitmap and no way to change the label from the manifest.
- Metadata is read once at boot, so installing a bundle means rebuilding
  the image (or re-running the gate); there is no installer and no way to
  remove an application from the desktop.
- A bundle has no private data directory, no saved settings and no way to
  persist a tool type change: the manifest is read-only data.
