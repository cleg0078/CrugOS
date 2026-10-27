#ifndef CRUG_TIMER_H
#define CRUG_TIMER_H

#include <crug/types.h>
#include <crug/trap.h>

int timer_init(void);
u64 timer_tsc_hz(void);
const char *timer_source(void);
u64 timer_ticks(void);
u64 timer_monotonic_ns(void);
u64 timer_tsc_to_us(u64 delta);
void timer_busy_wait_us(u64 us);
void timer_on_irq(trap_frame *frame);
int timer_selftest(void);

#endif
