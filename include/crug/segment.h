#ifndef CRUG_SEGMENT_H
#define CRUG_SEGMENT_H

/* Selectors installed by gdt_init() (kernel/arch/x86_64/cpu.c).
   The two user selectors carry RPL 3, so the same constant is valid both as
   a CS/SS value in an iretq frame and in segment-register loads. */
#define SEG_KERNEL_CS 0x08u
#define SEG_KERNEL_DS 0x10u
#define SEG_TSS       0x18u
#define SEG_USER_CS   0x2Bu
#define SEG_USER_DS   0x33u

/* GDT indices behind the selectors. */
#define GDT_INDEX_NULL      0
#define GDT_INDEX_KERNEL_CS 1
#define GDT_INDEX_KERNEL_DS 2
#define GDT_INDEX_TSS       3
#define GDT_INDEX_TSS_HIGH  4
#define GDT_INDEX_USER_CS   5
#define GDT_INDEX_USER_DS   6
#define GDT_ENTRIES         7

#endif
