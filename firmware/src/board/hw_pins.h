#ifndef HW_PINS_H
#define HW_PINS_H

/*
 * STM32U073CBT6 pin assignments — Saturn LoRa Tracker
 * Extracted from EasyEDA schematic (stm32_lora.eprj)
 *
 * Package: LQFP-48
 */

/* ---- SX1262 radio (E22-900M22S) — SPI1 ---- */
#define E22_NSS_PORT   GPIOA
#define E22_NSS_PIN    4    /* PA4  — chip select */

#define E22_SCK_PORT   GPIOA
#define E22_SCK_PIN    5    /* PA5  — SPI1_SCK  (AF5) */

#define E22_MISO_PORT  GPIOA
#define E22_MISO_PIN   6    /* PA6  — SPI1_MISO (AF5) */

#define E22_MOSI_PORT  GPIOA
#define E22_MOSI_PIN   7    /* PA7  — SPI1_MOSI (AF5) */

#define E22_DIO1_PORT  GPIOB
#define E22_DIO1_PIN   0    /* PB0  — interrupt */

#define E22_DIO2_PORT  GPIOB
#define E22_DIO2_PIN   1    /* PB1  */

#define E22_BUSY_PORT  GPIOB
#define E22_BUSY_PIN   2    /* PB2  — busy flag (input) */

#define E22_NRST_PORT  GPIOB
#define E22_NRST_PIN   3    /* PB3  — radio reset */

#define E22_RXEN_PORT  GPIOA
#define E22_RXEN_PIN   0    /* PA0  — RX antenna switch */

#define E22_TXEN_PORT  GPIOA
#define E22_TXEN_PIN   1    /* PA1  — TX antenna switch */

/* ---- GPS — USART2 ---- */
#define GPS_RX_PORT    GPIOA
#define GPS_RX_PIN     2    /* PA2  — USART2_TX→GPS_RX (AF7) */

#define GPS_TX_PORT    GPIOA
#define GPS_TX_PIN     3    /* PA3  — USART2_RX←GPS_TX (AF7) */

/* ---- SPI flash — SPI1 alt or bit-bang ---- */
#define FLASH_CLK_PORT GPIOB
#define FLASH_CLK_PIN  5    /* PB5 */

#define FLASH_DI_PORT  GPIOB
#define FLASH_DI_PIN   10   /* PB10 — MOSI to flash */

#define FLASH_DO_PORT  GPIOB
#define FLASH_DO_PIN   11   /* PB11 — MISO from flash */

#define FLASH_CS_PORT  GPIOB
#define FLASH_CS_PIN   12   /* PB12 — chip select */

/* ---- I2C1 (INA219 current sensor) ---- */
#define I2C_SCL_PORT   GPIOB
#define I2C_SCL_PIN    6    /* PB6  — I2C1_SCL (AF4) */

#define I2C_SDA_PORT   GPIOB
#define I2C_SDA_PIN    7    /* PB7  — I2C1_SDA (AF4) */

/* ---- I2C2 (INA219 on PB8/PB9, AF6) ---- */
#define INA_SCL_PORT   GPIOB
#define INA_SCL_PIN    8    /* PB8  — I2C2_SCL (AF6) */

#define INA_SDA_PORT   GPIOB
#define INA_SDA_PIN    9    /* PB9  — I2C2_SDA (AF6) */

/* ---- USB (DRD FS) ---- */
#define USB_DM_PORT    GPIOA
#define USB_DM_PIN     11   /* PA11 — USB_DM (AF10) */

#define USB_DP_PORT    GPIOA
#define USB_DP_PIN     12   /* PA12 — USB_DP (AF10) */

#define USB_VSENSE_PORT GPIOA
#define USB_VSENSE_PIN  9   /* PA9  — VBUS sense (analog/input) */

/* ---- LEDs (active high, 470 Ω series) ---- */
#define LED1_PORT      GPIOB
#define LED1_PIN       13   /* PB13 — LED1 */

#define LED2_PORT      GPIOB
#define LED2_PIN       14   /* PB14 — LED2 */

/* ---- Battery / power ---- */
#define BAT_SENSE_PORT GPIOB
#define BAT_SENSE_PIN  4    /* PB4  — ADC divider */

#define BAT_CHRG_PORT  GPIOA
#define BAT_CHRG_PIN   10   /* PA10 — charge status */

#define BAT_STDBY_PORT GPIOA
#define BAT_STDBY_PIN  8    /* PA8  — standby status */

#define SENSE_LDO_EN_PORT GPIOA
#define SENSE_LDO_EN_PIN  15  /* PA15 — LDO enable for sense divider */

/* ---- SWD (directly on MCU, active during debug) ---- */
#define SWDIO_PORT     GPIOA
#define SWDIO_PIN      13   /* PA13 — also LINK_DIO */

#define SWCLK_PORT     GPIOA
#define SWCLK_PIN      14   /* PA14 — also LINK_CLK */

/* ---- Boot ---- */
#define BOOT0_PORT     GPIOF
#define BOOT0_PIN      3    /* PF3 */

#endif /* HW_PINS_H */
