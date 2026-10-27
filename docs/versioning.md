# Versioning

CrugOS uses [Semantic Versioning 2.0.0](https://semver.org/) with annotated
`vMAJOR.MINOR.PATCH` Git tags. The current release is `v0.6.0` (phase 3B-2:
JBD2 journal replay and ordered metadata journaling).

## Source of truth

The repository file [`VERSION`](VERSION) is the only place a release number is
edited. The build reads it and passes `-DCRUG_VERSION_STR="<version>"` to the
kernel and the UEFI loader; `include/crug/version.h` supplies `0.0.0-dev` when
the compiler is invoked directly. The kernel prints the value on boot as
`CRUGOS: version <version>`, which the `make test` gate compares against
`VERSION`.

```
make version     # print the version the tree would build
```

## What the numbers mean

| Component | Bump when |
| --- | --- |
| MAJOR | a stable ABI breaks: the `boot_info` layout, the syscall dispatcher, the on-disk/handle interfaces, or the package format |
| MINOR | a phase or feature lands without breaking those ABIs |
| PATCH | bug fixes, documentation, build and test changes |

Pre-release suffixes (`0.2.0-rc1`) are allowed for release candidates. Once
`v1.0.0` exists, the kernel rejects a `boot_info` whose major version it does
not understand; before that, loader and kernel are always built and shipped
as a pair.

`BOOTINFO_VERSION` (currently 1) is versioned separately from the product: it
must be bumped whenever `struct boot_info` or a nested structure changes
layout or meaning, independently of whether the release is major, minor or
patch.

## Branches and tags

- `main` always builds and passes `make test`.
- Work happens on `feature/*` branches and reaches `main` by merge.
- A release is an annotated tag on `main`; tags are pushed explicitly.

## Release checklist

```
vi VERSION                       # pick the new SemVer
make distclean && make test      # clean build, boot gate, guest version check
make size check-efi test-host repro
make check-disk                  # e2fsck + persisted files on the test disk
git add -A && git commit -m "release: vX.Y.Z"
git tag -a vX.Y.Z -m "CrugOS vX.Y.Z"
git push origin main vX.Y.Z
```

## Released versions

| Version | Date | Contents |
| --- | --- | --- |
| `v0.1.0` | 2026-10-27 | phase 0: UEFI loader, ELF handoff, BootInfo, serial + framebuffer console, boot report, reproducible build and boot gates |
| `v0.2.0` | 2026-10-27 | phase 1: bitmap physical allocator, identity page tables with W^X, GDT/IDT and exceptions, LAPIC timer and TSC calibration, AVX2 baseline; BootInfo stays v1 |
| `v0.3.0` | 2026-10-27 | phase 2: kernel heap, threads and scheduling classes, handles, MsgPort IPC, single `int 0x80` syscall dispatcher; BootInfo stays v1 |
| `v0.4.0` | 2026-10-27 | phase 3A: block layer, legacy virtio-blk, GPT, read-only ext4, generated test disk; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.5.0` | 2026-10-27 | phase 3B-1: writable ext4 (create/mkdir/write/truncate/unlink/rmdir/rename/fsync), virtio flush, boot write self-test and `e2fsck` disk gate; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.6.0` | 2026-10-27 | phase 3B-2: JBD2 journal replay, ordered metadata journaling, dirty-journal mount recovery and a replay self-test; BootInfo stays v1 and the kernel ABI is unchanged |
