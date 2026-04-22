/*
 * stm32u0.h — STM32U073 register definitions for bare-metal drivers
 *
 * Covers RCC, GPIO, PWR, SPI1, USB DRD, CRS, SysTick, FLASH, SCB, NVIC.
 * All addresses from RM0503 reference manual.
 */

#ifndef STM32U0_H
#define STM32U0_H

#include <stdint.h>

/* ---- Cortex-M0+ system registers ---- */
#define SYST_CSR    (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR    (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR    (*(volatile uint32_t *)0xE000E018)

#define NVIC_ISER   (*(volatile uint32_t *)0xE000E100)
#define NVIC_ICER   (*(volatile uint32_t *)0xE000E180)
#define NVIC_ISPR   (*(volatile uint32_t *)0xE000E200)

/* SCB AIRCR for software reset */
#define SCB_AIRCR   (*(volatile uint32_t *)0xE000ED0C)

/* ---- RCC ---- */
#define RCC_BASE          0x40021000
#define RCC_CR            (*(volatile uint32_t *)(RCC_BASE + 0x00))
#define RCC_ICSCR         (*(volatile uint32_t *)(RCC_BASE + 0x04))
#define RCC_CFGR          (*(volatile uint32_t *)(RCC_BASE + 0x08))
#define RCC_AHBRSTR       (*(volatile uint32_t *)(RCC_BASE + 0x28))
#define RCC_IOPRSTR       (*(volatile uint32_t *)(RCC_BASE + 0x2C))
#define RCC_APBRSTR1      (*(volatile uint32_t *)(RCC_BASE + 0x38))
#define RCC_AHBENR        (*(volatile uint32_t *)(RCC_BASE + 0x48))
#define RCC_IOPENR        (*(volatile uint32_t *)(RCC_BASE + 0x4C))
#define RCC_APBENR1       (*(volatile uint32_t *)(RCC_BASE + 0x58))
#define RCC_APBENR2       (*(volatile uint32_t *)(RCC_BASE + 0x60))
#define RCC_CCIPR         (*(volatile uint32_t *)(RCC_BASE + 0x88))
#define RCC_CSR           (*(volatile uint32_t *)(RCC_BASE + 0x94))
#define RCC_CRRCR         (*(volatile uint32_t *)(RCC_BASE + 0x98))

/* ---- CRS ---- */
#define CRS_BASE          0x40006C00
#define CRS_CR            (*(volatile uint32_t *)(CRS_BASE + 0x00))
#define CRS_CFGR          (*(volatile uint32_t *)(CRS_BASE + 0x04))

/* ---- PWR ---- */
#define PWR_BASE          0x40007000
#define PWR_CR2           (*(volatile uint32_t *)(PWR_BASE + 0x04))

/* ---- GPIO ---- */
#define GPIOA_BASE        0x50000000
#define GPIOB_BASE        0x50000400
#define GPIOF_BASE        0x50001400

#define GPIO_MODER(base)   (*(volatile uint32_t *)((base) + 0x00))
#define GPIO_OTYPER(base)  (*(volatile uint32_t *)((base) + 0x04))
#define GPIO_OSPEEDR(base) (*(volatile uint32_t *)((base) + 0x08))
#define GPIO_PUPDR(base)   (*(volatile uint32_t *)((base) + 0x0C))
#define GPIO_IDR(base)     (*(volatile uint32_t *)((base) + 0x10))
#define GPIO_ODR(base)     (*(volatile uint32_t *)((base) + 0x14))
#define GPIO_BSRR(base)    (*(volatile uint32_t *)((base) + 0x18))
#define GPIO_AFRL(base)    (*(volatile uint32_t *)((base) + 0x20))
#define GPIO_AFRH(base)    (*(volatile uint32_t *)((base) + 0x24))

/* ---- SPI1 ---- */
#define SPI1_BASE         0x40013000
#define SPI1_CR1          (*(volatile uint32_t *)(SPI1_BASE + 0x00))
#define SPI1_CR2          (*(volatile uint32_t *)(SPI1_BASE + 0x04))
#define SPI1_SR           (*(volatile uint32_t *)(SPI1_BASE + 0x08))
#define SPI1_DR           (*(volatile uint32_t *)(SPI1_BASE + 0x0C))

/* SPI1_CR1 bits */
#define SPI_CR1_SPE       (1 << 6)
#define SPI_CR1_MSTR      (1 << 2)
#define SPI_CR1_SSI       (1 << 8)
#define SPI_CR1_SSM       (1 << 9)
#define SPI_CR1_BR_DIV16  (3 << 3)   /* 16MHz/16 = 1MHz */
#define SPI_CR1_BR_DIV8   (2 << 3)   /* 16MHz/8 = 2MHz */
#define SPI_CR1_BR_DIV4   (1 << 3)   /* 16MHz/4 = 4MHz */

/* SPI1_CR2 bits */
#define SPI_CR2_FRXTH     (1 << 12)  /* 8-bit RX threshold */
#define SPI_CR2_DS_8BIT   (7 << 8)   /* 8-bit data size */

/* SPI1_SR bits */
#define SPI_SR_RXNE       (1 << 0)
#define SPI_SR_TXE        (1 << 1)
#define SPI_SR_BSY        (1 << 7)

/* ---- USB DRD ---- */
#define USB_BASE          0x40005C00
#define USB_PMAADDR       0x40009800
#define PMA_BASE          USB_PMAADDR

#define USB_CHEP(n)       (*(volatile uint32_t *)(USB_BASE + 4*(n)))
#define USB_CNTR          (*(volatile uint32_t *)(USB_BASE + 0x40))
#define USB_ISTR          (*(volatile uint32_t *)(USB_BASE + 0x44))
#define USB_FNR           (*(volatile uint32_t *)(USB_BASE + 0x48))
#define USB_DADDR         (*(volatile uint32_t *)(USB_BASE + 0x4C))
#define USB_LPMCSR        (*(volatile uint32_t *)(USB_BASE + 0x54))
#define USB_BCDR          (*(volatile uint32_t *)(USB_BASE + 0x58))

/* BDT access */
#define BDT_TXBD(ep)      (*(volatile uint32_t *)(PMA_BASE + 8*(ep)))
#define BDT_RXBD(ep)      (*(volatile uint32_t *)(PMA_BASE + 8*(ep) + 4))

/* PMA buffer offsets */
#define EP0_TX_BUF        64
#define EP0_RX_BUF        128
#define EP1_TX_BUF        192
#define EP2_RX_BUF        256
#define EP3_TX_BUF        320

/* EP register bits */
#define USB_EP_CTR_RX     (1 << 15)
#define USB_EP_DTOG_RX    (1 << 14)
#define USB_EP_STAT_RX    (3 << 12)
#define USB_EP_SETUP      (1 << 11)
#define USB_EP_TYPE       (3 << 9)
#define USB_EP_KIND       (1 << 8)
#define USB_EP_CTR_TX     (1 << 7)
#define USB_EP_DTOG_TX    (1 << 6)
#define USB_EP_STAT_TX    (3 << 4)
#define USB_EP_EA         (0xF)

#define USB_EP_BULK       (0 << 9)
#define USB_EP_CONTROL    (1 << 9)
#define USB_EP_ISOCHRONOUS (2 << 9)
#define USB_EP_INTERRUPT  (3 << 9)

#define USB_EP_TX_DIS     (0 << 4)
#define USB_EP_TX_STALL   (1 << 4)
#define USB_EP_TX_NAK     (2 << 4)
#define USB_EP_TX_VALID   (3 << 4)
#define USB_EP_RX_DIS     (0 << 12)
#define USB_EP_RX_STALL   (1 << 12)
#define USB_EP_RX_NAK     (2 << 12)
#define USB_EP_RX_VALID   (3 << 12)

/* CNTR bits */
#define USB_CNTR_CTRM     (1 << 15)
#define USB_CNTR_PMAOVRM  (1 << 14)
#define USB_CNTR_ERRM     (1 << 13)
#define USB_CNTR_WKUPM    (1 << 12)
#define USB_CNTR_SUSPM    (1 << 11)
#define USB_CNTR_RESETM   (1 << 10)
#define USB_CNTR_SOFM     (1 << 9)
#define USB_CNTR_ESOFM    (1 << 8)

/* ISTR bits */
#define USB_ISTR_CTR      (1 << 15)
#define USB_ISTR_PMAOVR   (1 << 14)
#define USB_ISTR_ERR      (1 << 13)
#define USB_ISTR_WKUP     (1 << 12)
#define USB_ISTR_SUSP     (1 << 11)
#define USB_ISTR_RESET    (1 << 10)
#define USB_ISTR_SOF      (1 << 9)
#define USB_ISTR_ESOF     (1 << 8)
#define USB_ISTR_DIR      (1 << 4)
#define USB_ISTR_EP_ID    (0xF)

#define USB_BCDR_DPPU     (1 << 15)

/* RXBD encoding: BL_SIZE=1, NUM_BLOCK=1 => 64 bytes */
#define PMA_RXBD_BL_SIZE  (1U << 31)
#define PMA_RXBD_64       (PMA_RXBD_BL_SIZE | (1U << 26))

/* ---- FLASH ---- */
#define FLASH_BASE_REG    0x40022000
#define FLASH_ACR         (*(volatile uint32_t *)(FLASH_BASE_REG + 0x00))
#define FLASH_KEYR        (*(volatile uint32_t *)(FLASH_BASE_REG + 0x08))
#define FLASH_SR          (*(volatile uint32_t *)(FLASH_BASE_REG + 0x10))
#define FLASH_CR          (*(volatile uint32_t *)(FLASH_BASE_REG + 0x14))
#define FLASH_OPTR        (*(volatile uint32_t *)(FLASH_BASE_REG + 0x20))

/* Flash keys */
#define FLASH_KEY1        0x45670123
#define FLASH_KEY2        0xCDEF89AB

/* Flash SR bits */
#define FLASH_SR_BSY      (1 << 16)
#define FLASH_SR_EOP      (1 << 0)

/* Flash CR bits */
#define FLASH_CR_PG       (1 << 0)
#define FLASH_CR_PER      (1 << 1)
#define FLASH_CR_STRT     (1 << 16)
#define FLASH_CR_LOCK     (1 << 31)

/* EP RW bits mask */
#define EP_RW_BITS        (USB_EP_TYPE | USB_EP_KIND | USB_EP_EA)

#endif /* STM32U0_H */
