#ifndef CRUG_APIC_H
#define CRUG_APIC_H

#include <crug/types.h>

#define LAPIC_REG_ID        0x020u
#define LAPIC_REG_TPR       0x080u
#define LAPIC_REG_EOI       0x0b0u
#define LAPIC_REG_SVR       0x0f0u
#define LAPIC_REG_LVT_TIMER 0x320u
#define LAPIC_REG_TIMER_INIT 0x380u
#define LAPIC_REG_TIMER_CUR  0x390u
#define LAPIC_REG_TIMER_DIV  0x3e0u

u32 lapic_read(u32 offset);
void lapic_write(u32 offset, u32 value);
void lapic_eoi(void);
u32 lapic_id(void);
void apic_init(void);
void pic_mask_all(void);

#endif
