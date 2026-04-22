/*
 * Simple connectivity test for STM32U073CBT6
 * This minimal firmware:
 *  1. Runs from reset
 *  2. Writes a known pattern to a RAM location (for OpenOCD to verify)
 *  3. Toggles LED1 (PB13) and LED2 (PB14) on/off slowly
 *
 * If you can flash this and see LEDs toggle, your PCB is correct.
 */

#include <stdint.h>

/* RCC registers */
#define RCC_BASE        0x40021000
#define RCC_IOPENR      (*(volatile uint32_t *)(RCC_BASE + 0x4C))

/* GPIOB registers */
#define GPIOB_BASE      0x50000400
#define GPIOB_MODER     (*(volatile uint32_t *)(GPIOB_BASE + 0x00))
#define GPIOB_ODR       (*(volatile uint32_t *)(GPIOB_BASE + 0x14))
#define GPIOB_BSRR      (*(volatile uint32_t *)(GPIOB_BASE + 0x18))

/* Signature in RAM so OpenOCD can verify the MCU is alive */
#define ALIVE_MARKER    (*(volatile uint32_t *)0x20000000)
#define ALIVE_VALUE     0xDEADBEEF

static void delay(volatile uint32_t count)
{
    while (count--) {
        __asm__ volatile ("nop");
    }
}

void Reset_Handler(void)
{
    /* Write alive marker */
    ALIVE_MARKER = ALIVE_VALUE;

    /* Enable GPIOB clock */
    RCC_IOPENR |= (1 << 1);  /* GPIOBEN */

    /* Small delay for clock to stabilize */
    delay(100);

    /* Configure PB13 and PB14 as general purpose output (push-pull) */
    /* MODER: 00=input, 01=output, 10=alternate, 11=analog */
    /* PB13: bits [27:26] = 01 */
    /* PB14: bits [29:28] = 01 */
    uint32_t moder = GPIOB_MODER;
    moder &= ~((3 << 26) | (3 << 28));  /* Clear PB13 and PB14 mode bits */
    moder |=  ((1 << 26) | (1 << 28));  /* Set as output */
    GPIOB_MODER = moder;

    /* Blink LEDs alternately forever */
    while (1) {
        /* LED1 on, LED2 off */
        GPIOB_BSRR = (1 << 13);         /* Set PB13 */
        GPIOB_BSRR = (1 << (14 + 16));  /* Reset PB14 */
        delay(500000);

        /* LED1 off, LED2 on */
        GPIOB_BSRR = (1 << (13 + 16));  /* Reset PB13 */
        GPIOB_BSRR = (1 << 14);         /* Set PB14 */
        delay(500000);
    }
}

/* Minimal vector table */
extern uint32_t _estack;

__attribute__((section(".isr_vector")))
const uint32_t vectors[] = {
    (uint32_t)&_estack,       /* Initial stack pointer */
    (uint32_t)Reset_Handler,  /* Reset handler */
    (uint32_t)Reset_Handler,  /* NMI - just restart */
    (uint32_t)Reset_Handler,  /* HardFault - just restart */
};
