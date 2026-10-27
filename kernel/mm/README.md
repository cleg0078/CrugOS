# kernel/mm

Phase 0 deliberately contains no memory manager: the kernel runs directly on
the physical memory map supplied in `boot_info` and never allocates.

Planned for Phase 1:

- 4 KiB page allocator over the `CRUG_MEM_USABLE` regions with bitmap
  metadata stored above `__kernel_end`.
- Virtual memory with W^X: text read-only/executable, rodata read-only/NX,
  data/bss read-write/NX.
- Framebuffer memory (`CRUG_MEM_FRAMEBUFFER`) must never be handed to the
  allocator.
