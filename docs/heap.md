# The kernel heap

Phase H2 hardens the kernel heap in `kernel/mm/heap.c`. The allocator keeps the
same API and layout; what changed is that every structure it walks is now
validated before it is trusted, and the kernel can be asked whether the heap is
still consistent.

## The kernel heap

`kmalloc`, `kzalloc` and `kfree` serve eight slab classes from 16 to 2048
bytes. Each slab class owns whole `CRUG_PAGE_SIZE` pages; a request is rounded
up to its class and the first free object is taken from the slab free list.
Requests larger than 2048 bytes, up to the 1 MiB cap, become large blocks: a
contiguous run of PMM pages with a header at the start of the run.

The boot heap starts at step 7 of `kernel_main()`, before the block layer, GPT,
ext4 or any filesystem work, because every one of those stages allocates. The
same heap then backs the scheduler, message ports, the Workbench and the boot
report. A normal boot reaches the self-test with a handful of slab pages and, on
the gate configuration, one large block:

```
CRUGOS: heap check ok (6 slab(s), 8 object(s), 1 large block(s))
```

## Threat model

CrugOS is ring-0 only today, so a heap overflow or a use-after-free is not a
contained process fault: it corrupts the kernel itself. There is no MMU policy
that turns a bad write into a signal, and there is no allocator quarantine that
keeps a freed object unmapped. The H2 checker is a defence-in-depth detector for
buggy kernel code -- and, later, for corrupted metadata that reaches the heap
from an untrusted source. It is not a memory-safety guarantee, and it does not
make ring-0 code safe: a write inside a live object is invisible to it, and a
deliberate attacker who can already write kernel memory has won.

What the checker does buy is a loud, early failure instead of a silent walk
through a poisoned list: a forged header, a dangling free-list entry or a
counter that no longer matches the structures is reported with a specific code,
before the allocator follows the pointer.

## Page cookies

Every slab page starts with a `u64` cookie, and every large-block header starts
with one. The cookie is keyed by the address of the page it protects:

```
slab cookie  = HEAP_COOKIE_KEY ^ (uintptr)page ^ SLAB_COOKIE_SALT
large cookie = HEAP_COOKIE_KEY ^ (uintptr)page ^ LARGE_COOKIE_SALT

HEAP_COOKIE_KEY  = 0x435255474f5348ull   ("CRUGOSH")
SLAB_COOKIE_SALT = 0x534c4142ull         ("SLAB")
LARGE_COOKIE_SALT = 0x4c415247ull        ("LARG")
```

A header copied to, or forged at, the wrong address no longer produces the
cookie the address implies, so the first field checked rejects it. The cookie
also catches a header that was overwritten wholesale. It deliberately does not
authenticate the object payload: a value written inside a live object keeps the
page's cookie intact.

## Slab headers and the free list

`struct slab_header` is exactly 64 bytes, the same footprint as the phase 2
header, but every field is now checked before use:

```
struct slab_header {
    u64 cookie;
    u16 class_idx, objects, free_count, in_use;
    u64 used[4];            /* 256-bit object bitmap */
    void *free_list;
    struct slab_header *next;
};
```

`slab_validate()` implements the full check used by `heap_check()` and requires
all of the following:

- the cookie matches the page address and the slab salt;
- `class_idx` matches the slab class whose list the page is on;
- `objects` fits in the page after the header;
- `free_count + in_use == objects`;
- the `used` bitmap has no bit set at or past `objects`;
- every free-list entry points inside the page and is not marked allocated;
- the number of entries listed equals `free_count`, and the walk stops after
  at most `objects` entries, so a cycle is a failure rather than an endless
  loop.

On the allocation and free paths, `kmalloc` and `kfree` check the header they
touch: the cookie, the class index and the object counts, plus the free-list
entries they follow. `kmalloc` panics with `heap: corrupt slab header` when
those disagree, and with `heap: corrupt slab free list` when the next free
object is outside the page or is already allocated. Both panics fire before the
bad pointer is dereferenced.

## Large blocks

A large allocation carries `struct large_header`:

```
struct large_header {
    u64 cookie;
    u64 size, pages;
    void *next, *prev;
    u64 reserved[3];
};
```

The headers form a doubly linked list. Each link is checked against its
neighbour's `prev`/`next` pair, the cookie is checked against the header's page,
and `size`/`pages` must agree with the block's own accounting. `large_contains()`
answers whether a pointer falls anywhere inside a live large block, not just at
its start, so `free()` of an interior large pointer -- a pointer that the
allocator never returned -- panics with `heap: free of interior large pointer`
instead of corrupting the list.

## The integrity checker

`heap_check()` is read-only and can be called at any point where the caller can
afford to walk the heap:

```
int heap_check(u32 *slabs, u32 *objects, u32 *large);
const char *heap_check_name(u32 code);
```

It walks every slab class and the large list, fills the optional counts, and
returns a `HEAP_CHECK_*` code. It also cross-checks the running counters
`g_slab_pages`, `g_large_pages` and `g_large_bytes` against the live
structures, so a leak in the bookkeeping is reported even when every individual
header is valid.

| Code | Name | Meaning |
| --- | --- | --- |
| `HEAP_CHECK_OK` | `ok` | every structure validated |
| `HEAP_CHECK_SLAB_COOKIE` | `slab cookie` | a slab page cookie is wrong for its address |
| `HEAP_CHECK_SLAB_CLASS` | `slab class` | a slab header's class index disagrees with its list |
| `HEAP_CHECK_SLAB_COUNTS` | `slab counts` | object, free or in-use counts disagree |
| `HEAP_CHECK_SLAB_BITMAP` | `slab bitmap` | a used bit is set past `objects` |
| `HEAP_CHECK_SLAB_FREE_LIST` | `slab free list` | an entry is outside the page, allocated, or the count/cycle check failed |
| `HEAP_CHECK_LARGE_COOKIE` | `large cookie` | a large block cookie is wrong for its address |
| `HEAP_CHECK_LARGE_FIELDS` | `large fields` | a large header's size, page count or links disagree |
| `HEAP_CHECK_PAGE_TOTALS` | `page totals` | the global counters do not match the live structures |

`heap_check()` takes `cpu_irq_save()`/`cpu_irq_restore()` around the walks. The
boot-time call therefore cannot race the timer interrupt and observe a slab in
the middle of an allocation. The host build relies on the same wrapper: under
`CRUG_HOST_TEST` the interrupt helpers in `include/crug/cpu.h` return
`(1ull << 9)` and make `cpu_irq_restore()`/`cpu_irq_disable()` no-ops,
because `CLI`/`STI` are privileged instructions in user mode.

## Stress and guard modes

`heap_stress(u32 ops)` churns the allocator with a deterministic xorshift64
sequence seeded with `0x243f6a8885a308d3`: 55% allocations and 45% frees, at
most `HEAP_STRESS_LIVE` (64) live objects, sizes from 1 to 8000 bytes. Every
object is filled with a tag byte and verified before it is freed, `heap_check()`
runs every 64 operations, and the pass ends with a final check plus an exact
`heap_used_bytes()` accounting check. `ops` is capped by `HEAP_STRESS_MAX`
(200000), so a command-line token cannot turn a gate into an unbounded run.

`heap_guard()` plants three deliberate corruptions in real metadata -- a bad
free-list link, a zeroed slab cookie and a zeroed large-block cookie -- and
proves that `heap_check()` reports exactly the expected codes 5, 1 and 6. It
then repairs each one and verifies that the heap is clean again, so a guard run
leaves the allocator exactly as it found it.

Both modes are selected on the boot command line. `heap_cmdline()` parses them
right after `heap_init()` in `kernel/core/main.c`:

| Token | Meaning |
| --- | --- |
| `heap.stress=<1..200000>` | churn that many allocations/frees after the normal self-test; prints `CRUGOS: heap stress mode (N op(s))` |
| `heap.guard=1` | plant and repair the three corruptions; prints `CRUGOS: heap guard mode` |

`heap_selftest()` prints
`CRUGOS: heap check ok (S slab(s), O object(s), L large block(s))`, requires the
reuse check that a freed 16-byte object is handed back at the same address, and
runs the stress and guard passes when their tokens are set. Under
`CRUG_HOST_TEST`, `kernel/mm/heap.c` is compiled into `tests/host` so
`heap_check()` can be exercised on the host.

## The heap gate

`make test-heap` is the 45th gate. It boots with
`heap.stress=$(HEAP_STRESS_OPS) heap.guard=1`, checks the boot contract with
`EXPECT_HEAP=yes` and finally runs `scripts/check-disk.py`.

| Variable | Default | Meaning |
| --- | --- | --- |
| `EXPECT_HEAP` | `no` | `yes` requires the heap stress, guard and check markers |
| `HEAP_STRESS_OPS` | `4000` | operations the stress pass churns |
| `HEAP_RUN_TIMEOUT_SEC` | `240` | wall-clock budget for the gate boot |

A green run carries:

```
CRUGOS: heap stress mode (4000 op(s))
CRUGOS: heap guard mode
CRUGOS: heap check ok (6 slab(s), 8 object(s), 1 large block(s))
CRUGOS: heap stress ok (4000 op(s), 1850 alloc(s), 1850 free(s))
CRUGOS: heap guard detected corrupt free list (code 5)
CRUGOS: heap guard detected corrupt slab cookie (code 1)
CRUGOS: heap guard detected corrupt large cookie (code 6)
CRUGOS: heap guard ok (3 corruption(s) detected)
heap   : total=122880 B used=9040 B slab_pages=28 large_pages=2
```

and `boot-check.py` reports the line

```
version 0.41.0 avx2 yes raid no nvme no rebuild no ahci no crypt no aes any net no tcp no dns no usb no mouse no audio no gfx no wb no menu no shell no keys no desk no icons no drawer no select no heap yes user no isolated no ushell no spawn no fork no forkbig no storage disk QEMU exit status 33
```

followed by `ok: ext4 root is clean, /hello.txt and /selftest/b.txt persisted`
from `check-disk.py`. A failure is specific rather than a hang:
`CRUGOS: heap stress failed (…)`, `CRUGOS: heap guard missed corrupt … (code
N)` or `CRUGOS: heap check failed (%s, code %u)`, and the guest exits
non-zero.

## What is not covered

- There is no quarantine, no redzone and no guard page around an object, so a
  use-after-free that writes into a freed-and-reused slot is not detected.
- Leaked blocks are invisible to `heap_check()`: a block that is still linked
  and internally consistent is reported as fine even if no caller holds it.
- The cookie does not detect a write inside a live object; it protects the
  header, not the payload.
- `heap_guard()` corrupts real metadata and repairs it immediately, so it
  proves the checker's response, not that corruption is prevented.
- `heap_stress` is deterministic (fixed seed `0x243f6a8885a308d3` with an
  xorshift64), which is what makes the gate reproducible but also means it
  explores one fixed sequence.
- There is no host fuzzer of the allocation path yet; the host suite covers
  `heap_check()` against mirrored metadata, not random valid allocation
  sequences.
- The remaining `specs/fix-p1/REPORT.md` items are still open: per-CPU `rsp0`
  for ring 3, EFI entropy, and AHCI timeout bounce reuse. The non-`#BP` trap
  policy itself landed in phase H3 (see [trap policy](traps.md)).
