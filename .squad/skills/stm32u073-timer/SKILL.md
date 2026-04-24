---
name: "stm32u073-timer"
description: "SysTick-based 1 ms timebase on STM32U073 — polled, no ISR, delay_ms, beacon timer callback."
domain: "stm32-peripherals"
confidence: "high"
source: "earned"
---

## Context
No TIM peripheral is used for the system tick — the Cortex-M0+ core's **SysTick** is used in **polled** mode, matching the project's "no ISRs, polled main loop" architecture (ported from PIC24). Driver: `firmware/src/driver/timer.c/h`.

## Patterns

**Configuration (timer.c `timer_init()`):**
- `SYST_RVR = 16000 - 1` → 1 ms reload at 16 MHz processor clock
- `SYST_CVR = 0` (force reload on first tick)
- `SYST_CSR = (1<<2) | (1<<0)` = `CLKSOURCE=processor | ENABLE`. **TICKINT (bit 1) is deliberately 0** — no NVIC interrupt raised.

**Polling pattern (`timer_poll()`):** read-and-clear `SYST_CSR` bit 16 (COUNTFLAG). Reading CSR clears the flag atomically. On each tick, four software counters are incremented:
- `delayMs_t` (static, used by `delay_ms`)
- `difsTimer`, `slotTimer`, `ackTimer` (externally visible — consumed by maclayer.c for CSMA timing)
- `beaconTimer` (static, drives the 10 s beacon callback)

**Beacon callback:** `register_timer_cb(cb)` stores a function pointer + enable flag. `timer_poll()` fires it every 10000 ticks (10 s) and resets the counter. Only one callback slot — not a general-purpose scheduler.

**delay_ms (timer.c:57):** resets `delayMs_t=0` then `while (delayMs_t < t) timer_poll();`. This is the **only** place the user should call `delay_ms` — but the main loop MUST also call `timer_poll()` every iteration or the MAC timers stop advancing.

**Clock dependency:** assumes SYSCLK = 16 MHz HSI16. If `clock_init()` switches to PLL or HSI48 as SYSCLK, `SYST_RVR` must be updated — no runtime check.

**Typical main-loop shape (lora/main.c):**
```c
while (1) {
    timer_poll();        // advance tick counters
    usb_cdc_poll();      // service USB
    radio_irq_handler(); // poll DIO1 status
    mac_tick();
    network_tick();
    // ...
}
```

## Examples
- timer.c:22 `timer_init()` — SysTick config
- timer.c:31 `timer_poll()` — COUNTFLAG read and counter fan-out
- timer.c:57 `delay_ms()` — blocking delay (re-enters `timer_poll`, so MAC timers keep advancing during delays — intentional)
- lora/main.c `main()` loop — canonical poll order

## Anti-Patterns
- **Never** enable SysTick interrupt (`TICKINT=1`) — the project is polled. An ISR would need to be reconciled with the MAC layer's direct access to `difsTimer`/`slotTimer`/`ackTimer` (currently `volatile` but unprotected).
- Don't call `delay_ms()` from inside `radio_irq_handler()` or any path already under `timer_poll()` — the re-entrancy works, but nested 10 s beacon callbacks would stack.
- Don't call `timer_init()` twice — resets all counters silently.
- Don't read `SYST_CSR` multiple times to check COUNTFLAG — reading clears it. The single-read pattern in `timer_poll()` is intentional.
- Don't use `delay_ms` for fine timing (<2 ms) — polling granularity is 1 tick + loop overhead.
