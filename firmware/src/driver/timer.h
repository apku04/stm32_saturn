/*
 * timer.h — SysTick-based 1ms timebase for STM32U073
 */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

typedef void (*cb_timer)(void);

void     timer_init(void);
void     timer_poll(void);
void     delay_ms(uint32_t t);
uint32_t get_tick_ms(void);
void     register_timer_cb(cb_timer cb);

/* Timer variables used by MAC layer */
extern volatile uint32_t difsTimer;
extern volatile uint32_t ackTimer;
extern volatile uint32_t slotTimer;

#endif /* TIMER_H */
