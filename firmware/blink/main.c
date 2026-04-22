/*
 * LED Blink for STM32U073CBT6
 * LED1 = PB13 (through R33)
 * LED2 = PB14 (through R32)
 *
 * Pattern: alternating blink ~1Hz, then both together ~2Hz,
 * then a fast "heartbeat" pattern - confirms MCU is running well.
 */

#include <stdint.h>

/* RCC registers - STM32U073 */
#define RCC_BASE        0x40021000
#define RCC_IOPENR      (*(volatile uint32_t *)(RCC_BASE + 0x4C))

/* GPIOB registers */
#define GPIOB_BASE      0x50000400
#define GPIOB_MODER     (*(volatile uint32_t *)(GPIOB_BASE + 0x00))
#define GPIOB_OTYPER    (*(volatile uint32_t *)(GPIOB_BASE + 0x04))
#define GPIOB_OSPEEDR   (*(volatile uint32_t *)(GPIOB_BASE + 0x08))
#define GPIOB_ODR       (*(volatile uint32_t *)(GPIOB_BASE + 0x14))
#define GPIOB_BSRR      (*(volatile uint32_t *)(GPIOB_BASE + 0x18))

#define LED1_PIN  13
#define LED2_PIN  14

static void delay_ms(uint32_t ms)
{
    /* Approximate delay at ~4MHz MSI (default clock) */
    /* ~4000 iterations per ms at 4MHz with this loop */
    volatile uint32_t count = ms * 800;
    while (count--) {
        __asm__ volatile ("nop");
    }
}

static inline void led1_on(void)  { GPIOB_BSRR = (1 << LED1_PIN); }
static inline void led1_off(void) { GPIOB_BSRR = (1 << (LED1_PIN + 16)); }
static inline void led2_on(void)  { GPIOB_BSRR = (1 << LED2_PIN); }
static inline void led2_off(void) { GPIOB_BSRR = (1 << (LED2_PIN + 16)); }

static void init_leds(void)
{
    /* Enable GPIOB clock */
    RCC_IOPENR |= (1 << 1);

    /* Wait for clock */
    for (volatile int i = 0; i < 100; i++) __asm__("nop");

    /* PB13, PB14 as output push-pull */
    uint32_t moder = GPIOB_MODER;
    moder &= ~((3 << (LED1_PIN * 2)) | (3 << (LED2_PIN * 2)));
    moder |=  ((1 << (LED1_PIN * 2)) | (1 << (LED2_PIN * 2)));
    GPIOB_MODER = moder;

    /* Both LEDs off initially */
    led1_off();
    led2_off();
}

void Reset_Handler(void)
{
    init_leds();

    while (1) {
        /* Phase 1: Alternate blink (4 cycles) - confirms both LEDs work */
        for (int i = 0; i < 4; i++) {
            led1_on(); led2_off();
            delay_ms(300);
            led1_off(); led2_on();
            delay_ms(300);
        }

        /* Phase 2: Both blink together (4 cycles) */
        for (int i = 0; i < 4; i++) {
            led1_on(); led2_on();
            delay_ms(200);
            led1_off(); led2_off();
            delay_ms(200);
        }

        /* Phase 3: Heartbeat on LED1, LED2 steady (2 cycles) */
        led2_on();
        for (int i = 0; i < 2; i++) {
            led1_on();
            delay_ms(100);
            led1_off();
            delay_ms(100);
            led1_on();
            delay_ms(100);
            led1_off();
            delay_ms(500);
        }
        led2_off();
        delay_ms(500);
    }
}

/* Vector table */
extern uint32_t _estack;

__attribute__((section(".isr_vector")))
const uint32_t vectors[] = {
    (uint32_t)&_estack,
    (uint32_t)Reset_Handler,
    (uint32_t)Reset_Handler,  /* NMI */
    (uint32_t)Reset_Handler,  /* HardFault */
};
