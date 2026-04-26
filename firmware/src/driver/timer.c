/*
 * timer.c — SysTick-based 1ms timebase for STM32U073
 * Ported from PIC24 timerdriver.c
 *
 * Uses SysTick in polling mode (no ISR) — checked from main loop.
 * At 16MHz HSI16, SysTick reload = 16000 - 1 for 1ms ticks.
 */

#include "timer.h"
#include "stm32u0.h"

static volatile uint32_t delayMs_t   = 0;
static volatile uint32_t monotonic_ms = 0;
volatile uint32_t difsTimer   = 0;
volatile uint32_t ackTimer    = 0;
volatile uint32_t slotTimer   = 0;
static volatile uint32_t beaconTimer = 0;

static cb_timer beacon_cb    = 0;
static uint8_t  beacon_cb_en = 0;

/* Called once from main */
void timer_init(void)
{
    /* SysTick: processor clock (16MHz), reload for 1ms */
    SYST_RVR = 16000 - 1;
    SYST_CVR = 0;
    SYST_CSR = (1 << 2) | (1 << 0);  /* CLKSOURCE=processor, ENABLE */
}

/* Called from main loop — checks SysTick COUNTFLAG (bit 16 of CSR) */
void timer_poll(void)
{
    if (SYST_CSR & (1 << 16)) {
        /* 1ms tick */
        delayMs_t++;
        monotonic_ms++;
        difsTimer++;
        slotTimer++;
        ackTimer++;
        beaconTimer++;

        /* Beacon callback every 10 seconds */
        if (beacon_cb_en && beaconTimer >= 10000) {
            if (beacon_cb)
                beacon_cb();
            beaconTimer = 0;
        }
    }
}

/* Blocking delay using SysTick polling */
void delay_ms(uint32_t t)
{
    delayMs_t = 0;
    while (delayMs_t < t) {
        timer_poll();
    }
}

void register_timer_cb(cb_timer cb)
{
    beacon_cb    = cb;
    beacon_cb_en = 1;
}

uint32_t get_tick_ms(void)
{
    return monotonic_ms;
}
