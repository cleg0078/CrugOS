# Signed packages and capabilities

Phase 7B answers two questions at once: may this program run at all, and what
may it touch once it does? A package is a `.crpkg` file whose payload is
signed with Ed25519 and whose header carries an identity, a capability mask
and the hash of the payload. Installing one means verifying the signature
against `/System/trusted/keys.conf`, staging the files under
`/Applications`, renaming the stage into place and recording the result.
Running one means intersecting the manifest's requested capabilities with
`/System/perms.conf` and handing every ring-3 thread its own mask.

Phase 7A delivered the bundle format and the registry with no admission
control at all: anything on the disk was loadable. Phase 7B puts a lock on
the door and a list of what each caller may do after it is through.
`make test-pkg` is the 57th gate, `make test-caps` the 58th and
`make test-workbench-pkg` the 59th.

Phase 7C gives the installer a lifecycle. A newer package updates an older one
in place: the version is compared before anything is staged, the old bundle is
renamed to a backup, and the backup is deleted only once the new tree, the
record and the registry all agree. A package can be removed again, from the
shell or from the desktop window, and `SYS_SPAWN` and `SYS_EXEC` now require
`app.launch` and hand a child its parent's mask instead of `CAP_ALL`, which
closes phase 7B's largest open item. The gate count stays at 59 because each
phase's gate grew rather than adding another one.

The two mechanisms are deliberately separate. A signature proves the bytes
came from a key the operator trusts; it says nothing about what the program
does. Capabilities bound the damage a trusted program can do, including one
written carelessly or compromised at the source. Neither replaces the other,
and phase 7B implements both because either alone is a story rather than a
control.

## Why packages

Before phase 7B the root filesystem was a trust boundary with no gate. A
bundle under `/Applications` was accepted by scanning its directory, and a
`bin/x86_64` payload was a normal user ELF. Anyone who could write the image
could run anything. That was acceptable while the image was built by the same
repository that built the kernel, and untenable as soon as it was not.

The threat model is narrow and worth stating plainly:

- An unsigned package must never run. The kernel refuses to install one, so
  nothing extra reaches `/Applications`.
- A package signed by a key the operator did not list must never run. The
  trust list is the root of that decision, and it lives on the same
  read-only root filesystem the rest of the system boots from.
- A signed package must still not be able to do more than the policy allows.
  This is the capability half: the kernel no longer assumes that a program
  which was allowed to start is allowed to open, list, launch or connect.
- Nothing about the failure cases may leave a half-installed bundle behind.
  The stage directory exists so that a refusal has somewhere to be cleaned
  up from.

What phase 7B does not attempt is also explicit. There is no revocation, no
expiry, no online key distribution and no per-user policy; see the limits at
the end of this file.

## The capability model

A capability is a single bit in a 32-bit mask. Six are defined:

| Name | Bit | What it gates | Enforced by |
| --- | --- | --- | --- |
| `fs.read` | `0x01` | opening a file for reading, listing a directory | `SYS_OPEN`, `SYS_LISTDIR` |
| `fs.write` | `0x02` | opening a file for write, create or truncate | `SYS_OPEN` |
| `app.launch` | `0x04` | starting a bundle or a program | `SYS_APP_SPAWN`, `SYS_SPAWN`, `SYS_EXEC` |
| `net` | `0x08` | sockets and name resolution | `SYS_SOCKET_*`, `SYS_RESOLVE` |
| `audio` | `0x10` | reserved for a ring-3 audio path | none yet |
| `pkg.install` | `0x20` | installing, updating or removing a package | `SYS_PKG_INSTALL`, `SYS_PKG_REMOVE` |

`CAP_ALL` is `0x3F`: every defined bit. `cap_format()` renders a mask as
comma-joined names in table order and renders an empty mask as `none`, so the
two extremes are `fs.read,fs.write,app.launch,net,audio,pkg.install` and
`none`. `cap_parse_list()` accepts commas, spaces and tabs as separators and
requires capability tokens to be lowercase letters and dots; one bad token
fails the whole list, and the offending token is copied out for the error
message. An empty list parses successfully to `none`, which is what makes an
explicit grant of nothing writeable in a policy file.

Every thread carries a `u32 caps`. Kernel threads start with `CAP_ALL`;
ring-3 threads inherit the mask of whoever created them; a forked child
copies its parent's mask. There is no syscall to raise a mask, so the only
ways to get capabilities are to be kernel, to be a fresh child of a privileged
caller, or to be a bundle launched through the policy.

The rule for a launched bundle is one line in `kernel/app/app.c`:

```c
cap_intersect(a->m.cap_mask, perms_granted(perms_current(), a->m.id))
```

The manifest can only narrow the policy, and the policy can only narrow the
manifest. Install time and launch time are separate: a package may be
installed once and launched many times, and the policy is consulted on each
launch, so editing `/System/perms.conf` changes what happens on the next
boot without touching the package.

Enforcement happens at the syscall boundary, before any kernel state is
touched. `syscall_caps_ok()` returns 1 for a CPL0 caller and otherwise
requires every bit the call needs; a denial sets the return value to
`SYS_EPERM` (`-1`) and jumps to the end of the dispatch. A caller cannot
distinguish "you may not" from "it failed" by side effects, because none
happened.

The audio bit is defined, parsed, formatted and inherited but there is no
ring-3 audio syscall to gate yet, so it currently constrains nothing. That is
recorded again under the limits.

## The permissions policy

`/System/perms.conf` is a line-oriented text file read once at boot:

```
# CrugOS capability policy (phase 7B).
default app.launch,fs.read
captest fs.read
```

- Each line is `<id> <caps>`; `#` and blank lines are ignored.
- `default` sets the mask for every bundle without its own line.
- A bundle id is up to 31 characters of `[A-Za-z0-9._-]` and replaces the
  default for that id rather than adding to it.
- The capability list may be empty, which grants nothing.
- At most eight bundle rules (the default line is separate), and the file is
  read into a 1024-byte buffer.

A missing file is deliberately not an error: it grants `CAP_ALL`, which is
exactly the pre-7B behaviour, so a disk built without a policy still boots and
still runs its bundles. A file that exists but cannot be read or parsed does
the opposite: the kernel prints

```
CRUGOS: perms policy error (perms-cap)
CRUGOS: perms policy ready (default none, 0 rule(s))
```

and every bundle then launches with no capabilities. Failing closed on a
policy that is present but broken is the point of the distinction: an operator
who wrote a policy gets a policy.

| Reason | Meaning |
| --- | --- |
| `perms-line` | the file could not be read, or a line had no id |
| `perms-id` | the id has an illegal character or is too long |
| `perms-cap` | a capability token is unknown or malformed |
| `perms-dup` | a second `default` line |
| `perms-full` | a ninth bundle rule |

The boot always ends with

```
CRUGOS: perms policy ready (default app.launch,fs.read, 1 rule(s))
```

which reports the default that was actually installed, not the one the file
asked for, so a failed parse is visible in the boot log without reading the
error line.

## The manifest additions

Phase 7B adds two fields to the phase 7A manifest. `[app] publisher` names
who signed the bundle and is only validated as printable text; the signature
is what matters. `[caps]` carries exactly one key, `require`:

```toml
[app]
id = "captest"
name = "CapTest"
version = "1.0.0"
publisher = "Crug Labs"

[exec]
prog = "bin/x86_64/captest"

[caps]
require = "fs.read"
```

`[caps] require` is parsed by the same `cap_parse_list()` the policy uses, so
the syntax is identical; a bad list is `bad-cap` and a repeated key is
`dup-key`. A manifest without `[caps]` has an empty mask, which means a
bundle that requests nothing gets nothing even when the policy would allow
more. The rest of the grammar is in [apps.md](apps.md); only these two fields
changed.

The three things that now matter for an installed bundle are therefore the
signature in the package, the requirement in the manifest and the policy on
the root filesystem, and they are consulted in that order.

## The .crpkg container

A package is a 512-byte header followed by the payload. All integers are
little-endian.

| Offset | Size | Field |
| --- | --- | --- |
| `0x000` | 8 | magic `CRUGPKG1` |
| `0x008` | 4 | `header_size`, exactly 512 |
| `0x00C` | 4 | `flags`, must be 0 |
| `0x010` | 32 | package id |
| `0x030` | 32 | display name |
| `0x050` | 16 | version, three numeric parts |
| `0x060` | 32 | publisher, printable text |
| `0x080` | 4 | capability mask |
| `0x084` | 4 | payload size |
| `0x088` | 64 | SHA-512 of the payload |
| `0x0C8` | 32 | key id |
| `0x0E8` | 32 | Ed25519 public key |
| `0x108` | 8 | timestamp (informational) |
| `0x110` | 64 | Ed25519 signature |
| `0x150` | 176 | reserved, zero |

The text fields are fixed-size, NUL-terminated inside their window, and
validated character by character; the masks and sizes are range-checked, and
the payload size must be between 4 bytes and `PKG_MAX_BYTES` (128 KiB).
`header_size` must be exactly 512 and `flags` must be zero, which leaves room
for a future header version without giving this one any interpretation work.

The signature covers the first 512 bytes with the signature field temporarily
zeroed, so the header authenticates itself including the publisher, the
capability mask and the payload hash. The payload is authenticated indirectly
by its SHA-512, which is inside the signed region; one Ed25519 verification
therefore protects both.

The payload begins with `u32 file_count` and `u32 reserved`, followed by
that many entries of `u32 path_len`, `u32 data_len`, path bytes, data bytes.
Paths are relative, at most 95 characters, printable ASCII without
backslashes, colons, wildcards, a leading or trailing slash, an empty
component or a `..` component. The payload must contain `manifest.toml`; an
install without one is refused as `container`.

| Limit | Value |
| --- | --- |
| package file | 128 KiB |
| files per package | 32 |
| bytes per file | 96 KiB |
| path length | 95 characters |
| trust keys | 4 |
| policy rules | 8 plus the default |

## The signing tool

`tools/mkpkg.c` is a host program, built as `build/x86_64/mkpkg` and
reachable as `make mkpkg`. It walks a bundle directory, builds the payload,
hashes it, signs the header with an Ed25519 seed supplied on the command line
and writes one container:

```
mkpkg --seed HEX --out FILE --bundle DIR --id ID --name NAME --version V
      --publisher P [--caps LIST] [--key-id ID] [--timestamp N]
      [--trust-out FILE] [--print-key]
```

The seed is 64 hex characters, i.e. 32 bytes, and the public key is derived
from it. Making the seed an argument is what makes a test disk reproducible:
the signature is deterministic, so the same seed and the same payload produce
the same container byte for byte. `--trust-out` appends the matching trust
line, which lets one invocation seed both the package and the key list:

```
mkpkg: trust line appended to build/x86_64/pkg/keys.conf
mkpkg: public key 2f8d...c41a
```

`make packages` is the wrapper the image build uses. It builds the signer,
signs the test bundles, and writes four files into `build/x86_64/pkg`:

| File | Contents |
| --- | --- |
| `captest.crpkg` | a valid, trusted `captest` package |
| `untrusted.crpkg` | the same shape signed by a key the trust list does not hold |
| `keys.conf` | the trust list with the good key |
| `perms.conf` | `default app.launch,fs.read` plus `captest fs.read` |

`scripts/mkpkgtest.sh` builds the bundle tree for `captest` with a manifest
that requests only `fs.read`, signs both packages from the two seeds the
Makefile passes (`PKG_SEED` and `PKG_BAD_SEED`) and echoes one line:

```
mkpkgtest: build/x86_64/pkg/captest.crpkg build/x86_64/pkg/untrusted.crpkg build/x86_64/pkg/keys.conf build/x86_64/pkg/perms.conf
```

The two seeds are fixed in the Makefile, so the test packages are byte
identical across runs and the gates never depend on host entropy.

## The trust list

`/System/trusted/keys.conf` is one line per key:

```
# CrugOS trusted package keys (phase 7B).
pub 2f8d...c41a Crug Labs
```

The keyword is `pub`, followed by exactly 64 hex characters of public key and
a publisher name of at most 31 characters. At most four keys. A package is
trusted only when the key id and the embedded public key both appear on one
trusted line, so a package cannot borrow a trusted publisher's name for its
own key.

The list lives on the read-only root filesystem next to the packages it
vouches for, which is a real limitation: an attacker who can rewrite the image
can rewrite the list too. What phase 7B buys is that a package arriving
through any other path is worthless without the operator's key.

A missing or unreadable trust list trusts nothing and the kernel does not fail
the boot; a malformed list yields `trust-line`, `trust-hex`, `trust-key` or
`trust-full`, and the package that needed the failed entry is then refused as
`untrusted-key`. There is no separate "the trust list is broken" marker at
boot; the refusal of the first package that needed it is the signal.

## Installing a package

`pkg_install()` verifies first and mutates second:

```
  path
   |
   v
 [probe]      read the file, parse the header, verify the signature with the
   |          embedded key, verify the payload SHA-512, check the trust list
   v
 [stage]      create /Applications/.stage/<id>, write every payload file
   |          (0644), require manifest.toml, fsync
   v
 [rename]     rename the stage to /Applications/<id>.app
   |
   v
 [record]     write /System/pkgdb/<id>.txt
   |
   v
 [register]   add the bundle to the in-memory application registry
```

The root filesystem is mounted read-only for normal operation, so the
installer remounts it read-write for the mutation and read-only again
afterwards, including on the failure paths. Any failure after the rename
renames the bundle back to the stage and removes the stage tree, so the
visible state is always either "not installed" or "fully installed"; a record
that cannot be written removes the bundle again rather than leaving it
unregistered.

Before it stages anything the installer decides whether the package is new or
an update: it reads `/System/pkgdb/<id>.txt` and compares the header version
with the recorded one. The same version is refused as `already-installed`, an
older header as `downgrade`, and a newer header takes the update path
described under [Updating a package](#updating-a-package). Removal is the other
half of the lifecycle, described under [Removing a package](#removing-a-package).

Every refusal prints one line with a stable reason code, and the code has a
sentence form used by the desktop window:

```
CRUGOS: pkg rejected /System/packages/untrusted.crpkg (untrusted-key)
```

| Reason | Sentence printed in a window | Meaning |
| --- | --- | --- |
| `no-package` | no such package | the file cannot be opened |
| `io` | filesystem error | a read, write, rename or unlink failed |
| `too-large` | package too large | the file exceeds 128 KiB |
| `short` | truncated header | the file is not even 512 bytes |
| `magic` | not a CrugOS package | the first eight bytes are not `CRUGPKG1` |
| `header-size` | bad header size | `header_size` is not 512 |
| `flags` | unsupported flags | `flags` is not zero |
| `id` | bad package id | the id is empty, too long or has illegal characters |
| `name` | bad package name | the name is empty or unprintable |
| `version` | bad version | not three numeric parts |
| `publisher` | bad publisher | empty or unprintable |
| `payload-size` | bad payload size | zero, under 4 bytes or over 128 KiB |
| `cap` | bad capability set | a bit outside `CAP_ALL` |
| `key` | bad key id | the key id field is empty or malformed |
| `trust-line` | bad trust list | a trust line is not `pub <hex> <name>` |
| `trust-hex` | bad trust list | the key is not 64 hex characters |
| `trust-full` | bad trust list | a fifth key |
| `untrusted-key` | untrusted signing key | the key is not in the trust list |
| `signature` | bad signature | Ed25519 verification failed |
| `payload-hash` | payload does not match its hash | SHA-512 mismatch |
| `container` | damaged payload | the file table is inconsistent or `manifest.toml` is missing |
| `path` | illegal file path | absolute, `..`, empty component, bad characters |
| `file-size` | file too large | one payload file exceeds 96 KiB |
| `too-many-files` | too many files | more than 32 payload files |
| `already-installed` | already installed | the same version is already installed |
| `downgrade` | newer version installed | the installed version is newer than the header |
| `read-only` | the root is read-only | the remount to read-write failed |
| `not-installed` | package not installed | a remove for an id that is neither installed nor on disk |
| `record` | cannot record install | the pkgdb record could not be written |

Three states are reported by the installer on success:

```
CRUGOS: pkg installed captest 1.0.0 (2 file(s), 1 cap(s))
CRUGOS: pkg updated captest 1.0.0 -> 2.0.0 (2 file(s), 1 cap(s))
CRUGOS: pkg removed captest (0 package(s))
```

A failed removal prints `CRUGOS: pkg remove rejected <id> (<reason>)`; a
failed install keeps the older `pkg rejected` line. At boot, once the pkgdb
records have been read back:

```
CRUGOS: package registry ready (1 package(s))
```

The count after the comma is the header's capability mask as a number, not the
number of capability names; it is the mask the package asked for, not the mask
it will actually receive.

At boot `pkg_init()` scans `/System/pkgdb` for `*.txt` records, parses
`id|name|version|publisher|caps|prog` and inserts them into a sorted static
registry. The bundle registration that follows reads `/Applications` as it did
in phase 7A; an installed bundle therefore appears in both tables and can be
launched from the desktop and the shell exactly like a hand-built one.

## Updating a package

An install is an update when the version in the header is newer than the one in
the pkgdb record for the same id. The comparison is `app_version_compare()`
over the three numeric parts, so `1.0.0` to `2.0.0` is an update, `2.0.0`
over `1.0.0` is a `downgrade` refusal and the same version is
`already-installed`. Everything else on the install path is unchanged: the
signature, the payload hash, the trust list and the manifest rules are checked
before the update is considered, and the new header must be signed by a key
the trust list carries, exactly like a first install. The id must match, which
it does by construction - the record is looked up by the id in the header.

The swap is the only extra state the installer keeps:

1. stage the new tree in `/Applications/.stage/<id>` and fsync it, as for a
   fresh install;
2. remove any leftover backup, then rename the installed tree to
   `/Applications/.stage-<id>.old`;
3. rename the stage to `/Applications/<id>.app`;
4. rewrite the pkgdb record, fsync it and re-scan the registry;
5. remove the backup tree and its directory, which is the point of no return.

Every failure after step 2 puts the old tree back: a failed rename of the stage
renames the backup to its original name, and a failed record renames the new
tree back to the stage and the backup to the installed name, so the visible
state is the old package, fully working. A failure before the first rename
only removes the stage tree. The success marker names both versions:

```
CRUGOS: pkg updated captest 1.0.0 -> 2.0.0 (2 file(s), 1 cap(s))
```

An update replaces the bundle directory wholesale. Files the new payload does
not carry do not survive, and nothing outside `/Applications/<id>.app` is
touched.

## Removing a package

`pkg_remove(const char *id, const char **reason)` is the inverse of install.
It validates the id (letters, digits, dot, underscore and dash, at most 31
characters) and requires either a bundle directory or a registry entry, so a
remove of an id that is not installed is refused as `not-installed` rather
than being treated as success. It then:

1. remounts the root read-write, or refuses with `read-only`;
2. deletes `/Applications/<id>.app` recursively;
3. unlinks `/System/pkgdb/<id>.txt`;
4. fsyncs the filesystem;
5. drops the registry entry, re-runs the bundle scan so the desktop and the
   shell stop listing it, remounts read-only and prints

```
CRUGOS: pkg removed captest (0 package(s))
```

The count in brackets is the package registry size after the removal, so the
line doubles as a check that the record went away. A refusal prints
`CRUGOS: pkg remove rejected <id> (<reason>)` with one of `id`,
`not-installed`, `read-only` or `io`. Removal does not touch anything the
program wrote outside its bundle, and there is no uninstall script: the
package format has no way to describe one.

## The syscall ABI

| Number | Name | Arguments | Returns |
| --- | --- | --- | --- |
| 28 | `SYS_CAP_QUERY` | none | the caller's mask |
| 29 | `SYS_CAP_CHECK` | `rdi` = required mask | 1 when every bit is held, else 0 |
| 30 | `SYS_PKG_COUNT` | none | number of installed packages |
| 31 | `SYS_PKG_INFO` | `rdi` index, `rsi` buffer, `rdx` capacity | bytes written, or a negative error |
| 32 | `SYS_PKG_INSTALL` | `rdi` path | 0, or a negative error |
| 33 | `SYS_PKG_REMOVE` | `rdi` id | 0, or a negative error |

`SYS_MAX` is 34. A package record is at most 192 bytes and has the same shape
as the pkgdb file:

```
id|name|version|publisher|caps|prog
captest|CapTest|1.0.0|Crug Labs|fs.read|bin/x86_64/captest
```

`SYS_PKG_INFO` clamps the capacity to 192, rejects a null buffer or zero
capacity with `SYS_EFAULT`, an out-of-range index with `SYS_ENOENT` and a
record that does not fit with `SYS_EINVAL`. `SYS_PKG_INSTALL` checks
`CAP_PKG_INSTALL` first, so a bundle without it gets `SYS_EPERM` and the
installer never runs; with the capability, a failed install returns
`SYS_EIO` and the reason appears in the kernel log, not in the return value.
`SYS_PKG_REMOVE` shares the same bit and takes the package id rather than a
path: without `pkg.install` it returns `SYS_EPERM`, and when `pkg_remove()`
refuses the call returns `SYS_EIO` with the reason in the log. It is the only
removal path; there is no unlink-style interface.
`SYS_CAP_CHECK` is a query, not an enforcement point: it answers 0 rather
than an error, and the caller decides what to do about it.

The ring-3 side of the ABI is a set of thin wrappers in `user/lib.h`:
`u_cap_query()`, `u_cap_check(mask)`, `u_pkg_count()`,
`u_pkg_info(index, buf, len)`, `u_pkg_install(path)` and
`u_pkg_remove(id)`, plus the `U_CAP_*` bit macros. The shell has a `pkg`
builtin with `list`, `info <id>`, `install <path>` and `remove <id>`
subcommands; its other builtins are `ver`, `echo`, `cat`, `ls`, `write`,
`app` and `exit`. The `pkg` builtin prints `CRUGOS: sh pkg count <n>` and one
`CRUGOS: sh pkg <id> <version>` line per package for `list`, and
`CRUGOS: sh pkg installed <path>` or `CRUGOS: sh pkg removed <id>` for the
mutations.

## The selftests

`pkg.test=1` (gate 57) proves the install path and the trust boundary:

```
CRUGOS: pkg selftest start
CRUGOS: pkg selftest probe captest 1.0.0 trusted
CRUGOS: pkg selftest probe untrusted rejected (untrusted-key)
CRUGOS: pkg selftest installed captest 1.0.0
CRUGOS: pkg selftest updated captest 1.0.0 -> 2.0.0
CRUGOS: pkg selftest rejected downgrade (downgrade)
CRUGOS: pkg selftest rejected second install (already-installed)
CRUGOS: pkg selftest removed captest (0 package(s))
CRUGOS: pkg selftest rejected second remove (not-installed)
CRUGOS: pkg selftest rejected untrusted (untrusted-key)
CRUGOS: pkg selftest app registered captest
CRUGOS: pkg selftest ok (2 installed, 1 updated, 1 removed)
```

It probes both packages without installing, installs the trusted 1.0.0, checks
that its payload and manifest are on disk, updates it to 2.0.0 with the second
package, proves that 1.0.0 is now refused as a downgrade and the same version
as already installed, removes the package, proves a second remove is refused,
proves the untrusted package is refused even though its payload is well formed,
and re-runs the application scan. It ends by reinstalling 1.0.0, so the caps
level and the gates still find the id they expect. `scripts/mkpkgtest.sh`
builds `captest2.crpkg` by rewriting the manifest version to 2.0.0 and
keeping the seed, so both packages carry the same key and publisher.

`pkg.test=2` (gate 58) proves the capability wall. It installs `captest` if
the previous level did not, computes the granted mask, launches the bundle and
checks that the child got exactly `fs.read`:

```
CRUGOS: caps selftest start
CRUGOS: caps selftest manifest 0x1 policy 0x1 granted 0x1
CRUGOS: caps selftest launched tid 12 caps 0x1
CRUGOS: captest ok
CRUGOS: captest denied fs.write
CRUGOS: captest denied pkg.install
CRUGOS: captest denied pkg.remove
CRUGOS: captest denied spawn
CRUGOS: captest denied exec
CRUGOS: caps selftest ok (exit 5, caps 0x1)
```

`/bin/captest` is the probe: it prints its mask, confirms `fs.read` is held
and `fs.write` is not, reads `/hello.txt`, requires a write-open of
`/System/captest-denied` to fail, and requires `u_pkg_install()`,
`u_pkg_remove()`, `u_spawn()` and `u_exec()` to fail. The last two are the
phase 7C check that a narrowed program cannot widen itself by launching another
image. A wrong mask or an operation that succeeds produces a different
failure, so the gate names the wall that broke.

The token is parsed next to the other boot tokens; `pkg.test=2` installs the
bundle itself when `pkg.test=1` did not run first, so the two gates do not
depend on each other's order.

## The desktop installer

`wb.pkg=1` opens a `Packages` window beside the Workbench on the 1024x768
desktop:

- geometry `(40, 330)`, `420` by `200`, title `Packages`, body
  `CrugOS Packages`;
- a subtitle naming the package a click will install;
- one 18-pixel row per installed package, id on the left and version on the
  right;
- one press anywhere in the body toggles `/System/packages/captest.crpkg`: it
  installs when the id is absent, removes it when it is installed, and paints
  `installed` or `removed` in amber, or paints the reason sentence on
  failure.

The window is opened during `wb_init()`, before `pkg_init()` reads the
registry, so it starts empty and fills on the first redraw after boot. That
ordering is visible in the log: `CRUGOS: wb pkg ready (window 3, 0
package(s))` comes before the package registry line.

The markers are:

```
CRUGOS: wb pkg ready (window 3, 0 package(s))
CRUGOS: wb pkg installed /System/packages/captest.crpkg (1 package(s))
CRUGOS: wb pkg removed captest (0 package(s))
CRUGOS: wb pkg rejected <file> (<reason>)
CRUGOS: wb pkg selftest ok (2 install(s), 1 remove(s), 1 package(s))
```

Each press looks the id up in the registry first: an installed id is removed
through `pkg_remove()`, an absent one is installed. The body then paints
`removed` or `installed` until the next redraw, so a second press is a
second operation rather than a no-op, and the gate can press three times to
watch the install/remove/install cycle.

## The gates

`make packages` is not a gate; it is the build step the package gates and
`make disk` depend on. It signs the test bundles and writes the four files
described above.

`make test-pkg` (gate 57) rebuilds the disk, boots with
`CMDLINE_EXTRA="pkg.test=1"`, runs `boot-check.py --expect-pkg=yes` and
finishes with `check-disk.py`. The boot-check line reports which optional
fields were expected:

```
ok: boot markers found, version 0.45.0 avx2 yes ... app no pkg yes caps no wbpkg no heap no user no ... storage disk QEMU exit status 33
```

The same leg covers the lifecycle now: the selftest installs 1.0.0, updates
to 2.0.0, refuses the downgrade, removes the package and refuses the second
remove, so gate 57 is the update and removal gate as well as the install gate.

`make test-caps` (gate 58) does the same with `pkg.test=2` and
`--expect-caps=yes`, and additionally requires the `captest` markers and
`caps selftest ok (exit 5, caps 0x1)`.

`make test-workbench-pkg` (gate 59) is the desktop leg. It rebuilds the
disk, boots with `CMDLINE_EXTRA="wb.enable=1 wb.wait=<WB_PKG_WAIT>
wb.pkg=1 wb.acts=3"` and drives the window with a QEMU-QMP hot-plugged
mouse:

1. wait for `wb desktop ready` and `wb pkg ready`;
2. add `usb-mouse` on the xHCI controller and wait for the cursor;
3. screendump before the first click and require zero amber pixels in the
   window body, i.e. no `installed` line yet;
4. click the body at `(250, 430)`, wait for the install marker, press again
   and wait for the remove marker, press a third time and wait for the second
   install;
5. screendump after the clicks and require amber pixels;
6. run `boot-check.py --expect-wbpkg=yes`, then `check-disk.py`.

The driver's command line hardcodes `wb.acts=3`, so the window completes
exactly the install/remove/install cycle the selftest watches for. On success
it prints:

```
ok: workbench package toggle (2 installs, 1 remove, exit 33)
```

A failure names the step, for example `FAIL: the window body is already
highlighted: 12 pixel(s)` or `FAIL: the installed line is missing from the
window body`. The first means the disk already carried an installed package,
which points at a stale image rather than at the kernel; the second means the
click landed but the install did not, so the serial log holds the reason.

The knobs are `WB_PKG_WAIT` (30 s), `WB_PKG_RUN_TIMEOUT_SEC` (240 s),
`PKG_RUN_TIMEOUT_SEC` (240 s), `CAPS_RUN_TIMEOUT_SEC` (240 s), `MKPKG`,
`PKG_DATA_DIR`, `PKG_SEED` and `PKG_BAD_SEED`. The seeds are the hex strings
in the Makefile, so overriding them produces different keys and signatures and
invalidates the trust list unless it is rebuilt too; they exist for
inspection, not for normal runs.

## Failure modes

| Symptom | Likely cause | Where it shows |
| --- | --- | --- |
| `pkg rejected ... (untrusted-key)` | the key is not in `/System/trusted/keys.conf`, or the trust list is missing | serial log, desktop window |
| `pkg rejected ... (signature)` | the header was modified after signing, or the wrong seed signed it | serial log |
| `pkg rejected ... (payload-hash)` | a payload byte changed, including a truncation that kept the sizes consistent | serial log |
| `pkg rejected ... (container)` | the file table is inconsistent, or `manifest.toml` is missing from the payload | serial log |
| `pkg rejected ... (already-installed)` | the same version is already installed | serial log |
| `pkg rejected ... (downgrade)` | the installed version is newer than the package | serial log |
| `pkg remove rejected ... (not-installed)` | the id is neither in the registry nor on disk | serial log |
| `pkg rejected ... (read-only)` | the root could not be remounted read-write | serial log |
| `pkg rejected ... (record)` | the pkgdb record could not be written; the bundle was removed again | serial log |
| `perms policy error (...)` | the policy file is unreadable or malformed | serial log; the ready line shows `default none` |
| `perms policy ready (default none, ...)` with no error line | the file legitimately grants nothing | serial log |
| `captest denied fs.write` missing from a caps gate | the policy granted more than the manifest asked, or the manifest asked for more than the policy allows | the gate failure names the child's exit code |
| `wb pkg rejected ...` after a click | the package on the image is stale or signed with the wrong seed | serial log after the driver's click |
| a bundle runs but every syscall returns `-1` | it was launched with an empty mask: no `[caps] require`, or a policy that grants nothing | `SYS_CAP_QUERY`, the program's own checks |

Two of these are design choices rather than bugs. An update replaces the
installed bundle wholesale, so files the old version carried but the new one
does not are gone afterwards; there is no file-level merge and no dependency
resolution. And an empty mask is a usable state, not a broken one, which is why
the policy file may grant `none` explicitly.

## Limits

- No revocation and no expiry. The timestamp in the header is recorded and
  never checked; a key removed from the trust list stops new installs and does
  nothing about packages already installed.
- No re-verification at launch. The signature is checked once, at install
  time; the payload on disk after that is trusted, and nothing detects a
  modified `/Applications/<id>.app` tree.
- The trust list is an ordinary file on the same read-only filesystem as the
  packages. An operator who can change the image can change both; the
  signature protects the path into the system, not the system itself.
- No per-user policy and no runtime policy engine: one file, eight rules, one
  default, read once at boot. A bundle line replaces the default rather than
  extending it, so it must repeat anything it wants to keep.
- The capability table is fixed at six bits and a 32-bit mask. The audio bit
  is defined but no syscall gates it yet, because there is no ring-3 audio
  path to gate.
- Capability checks sit at the listed syscalls only. `SYS_SPAWN` and
  `SYS_EXEC` require `app.launch` and both hand the child the caller's mask,
  so attenuation now covers every launch path; a bundle with an empty mask can
  no longer start a `/bin` program to recover the full set. `CAP_AUDIO` is
  still defined and unchecked.
- No dependency resolution, no file-level merge on update, and no rollback
  beyond the stage/backup swap: an interrupted update leaves either the old
  bundle or the new one, never a mixture, but nothing records a history that
  could be rolled back later.
- Four trust keys, 32 files, 96 KiB per file, 128 KiB per package, 8 policy
  rules. The package is read in one allocation bounded by `PKG_MAX_BYTES`, so
  a bigger one is refused, not streamed.
- Signing happens on the host with a seed on the command line. That is right
  for reproducible test images and wrong for real key handling, where a
  private key should never be an argument.
- The desktop installer is a demo: one hard-coded path, one package id, a
  single body click that toggles the id. A real installer needs a file picker,
  a progress path and a package list rather than one row.
