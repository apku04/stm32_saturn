/*
 * ext_flash.c — W25Q32 external SPI flash driver (bit-bang SPI)
 *
 * Pins (from hw_pins.h):
 *   PB5  = CLK
 *   PB10 = DI  (MOSI — data INTO the flash)
 *   PB11 = DO  (MISO — data OUT of the flash)
 *   PB12 = CS  (active low)
 *
 * Bit-bang SPI mode 0 (CPOL=0, CPHA=0).
 * Clock ~1-2 MHz at 16 MHz HSI (tuned by NOP count).
 */

#include "ext_flash.h"
#include "stm32u0.h"
#include "hw_pins.h"

/* ---- Pin helpers ---- */

#define CLK_PIN   FLASH_CLK_PIN   /* PB5  */
#define MOSI_PIN  FLASH_DI_PIN    /* PB10 */
#define MISO_PIN  FLASH_DO_PIN    /* PB11 */
#define CS_PIN    FLASH_CS_PIN    /* PB12 */

#define BB_DELAY()  do { for (volatile int _i = 0; _i < 4; _i++) __asm__("nop"); } while (0)

static inline void cs_low(void)   { GPIO_BSRR(GPIOB_BASE) = (1u << (CS_PIN + 16)); }
static inline void cs_high(void)  { GPIO_BSRR(GPIOB_BASE) = (1u << CS_PIN); }
static inline void clk_low(void)  { GPIO_BSRR(GPIOB_BASE) = (1u << (CLK_PIN + 16)); }
static inline void clk_high(void) { GPIO_BSRR(GPIOB_BASE) = (1u << CLK_PIN); }
static inline void mosi_low(void) { GPIO_BSRR(GPIOB_BASE) = (1u << (MOSI_PIN + 16)); }
static inline void mosi_high(void){ GPIO_BSRR(GPIOB_BASE) = (1u << MOSI_PIN); }
static inline uint8_t miso_read(void) {
    return (uint8_t)((GPIO_IDR(GPIOB_BASE) >> MISO_PIN) & 1u);
}

/* ---- W25Q commands ---- */

#define CMD_WRITE_ENABLE   0x06
#define CMD_WRITE_DISABLE  0x04
#define CMD_READ_STATUS1   0x05
#define CMD_READ_DATA      0x03
#define CMD_PAGE_PROGRAM   0x02
#define CMD_SECTOR_ERASE   0x20   /* 4 KB */
#define CMD_BLOCK_ERASE    0xD8   /* 64 KB */
#define CMD_CHIP_ERASE     0xC7
#define CMD_JEDEC_ID       0x9F

#define STATUS_BUSY        0x01

/* ---- Low-level SPI transfer ---- */

static uint8_t bb_spi_transfer(uint8_t tx)
{
    uint8_t rx = 0;
    for (int i = 7; i >= 0; i--) {
        /* Set MOSI */
        if (tx & (1u << i))
            mosi_high();
        else
            mosi_low();
        BB_DELAY();

        /* Rising edge — flash samples MOSI, we sample MISO */
        clk_high();
        BB_DELAY();
        if (miso_read())
            rx |= (1u << i);

        /* Falling edge */
        clk_low();
    }
    return rx;
}

static void bb_spi_send(uint8_t b)
{
    (void)bb_spi_transfer(b);
}

static uint8_t bb_spi_recv(void)
{
    return bb_spi_transfer(0xFF);
}

/* ---- Internal helpers ---- */

static void flash_write_enable(void)
{
    cs_low();
    bb_spi_send(CMD_WRITE_ENABLE);
    cs_high();
}

static uint8_t flash_read_status(void)
{
    cs_low();
    bb_spi_send(CMD_READ_STATUS1);
    uint8_t s = bb_spi_recv();
    cs_high();
    return s;
}

/*
 * Wait until the chip clears its BUSY bit.
 *
 * Returns 0 on success, -1 on timeout. The caller is allowed to ignore the
 * return code for short ops (page-program ≤3 ms, sector-erase ≤400 ms),
 * but ext_flash_init() should check it so a missing/dead chip cannot brick
 * the boot sequence.
 *
 * Implementation notes:
 *   - The poll loop is bounded by an iteration counter, NOT wall time, so
 *     this works before timer_init() runs (init order: spi=4, ..., flash=9).
 *   - We refresh the IWDG inside the loop. W25Q32 chip-erase can take up to
 *     ~25 s typical / 100 s max — much longer than the 8 s watchdog window.
 *   - IWDG_KR=0xAAAA is the standard refresh; the watchdog is already
 *     started by main() before any flash op happens.
 *
 * Timeout sizing: 0x4000000 iterations ≈ a few seconds of polling at 16 MHz
 * with the function-call overhead of flash_read_status(). Long enough for
 * sector erase, short enough that init can't wedge the board.
 */
static int flash_wait_busy_timeout(uint32_t max_iters)
{
    uint32_t i = 0;
    while (flash_read_status() & STATUS_BUSY) {
        /* Refresh IWDG so a long erase doesn't trigger a watchdog reset. */
        IWDG_KR = IWDG_KR_REFRESH;
        if (++i >= max_iters)
            return -1;
    }
    return 0;
}

/* Default: large budget — used by program/sector-erase paths where we are
 * confident the chip is alive. Block-erase / chip-erase callers may want a
 * larger value. */
static void flash_wait_busy(void)
{
    (void)flash_wait_busy_timeout(0x4000000u);
}

static void flash_send_addr(uint32_t addr)
{
    bb_spi_send((uint8_t)(addr >> 16));
    bb_spi_send((uint8_t)(addr >> 8));
    bb_spi_send((uint8_t)(addr));
}

/* ---- Public API ---- */

static int s_present = 0;

int ext_flash_present(void) { return s_present; }

void ext_flash_init(void)
{
    /* Enable GPIOB clock */
    RCC_IOPENR |= (1u << 1);
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    /* CS (PB12) — output, push-pull, start high (deselected) */
    uint32_t moder = GPIO_MODER(GPIOB_BASE);
    moder &= ~(3u << (CS_PIN * 2));
    moder |=  (1u << (CS_PIN * 2));
    GPIO_MODER(GPIOB_BASE) = moder;
    cs_high();

    /* CLK (PB5) — output, push-pull, start low */
    moder = GPIO_MODER(GPIOB_BASE);
    moder &= ~(3u << (CLK_PIN * 2));
    moder |=  (1u << (CLK_PIN * 2));
    GPIO_MODER(GPIOB_BASE) = moder;
    clk_low();

    /* MOSI (PB10) — output, push-pull */
    moder = GPIO_MODER(GPIOB_BASE);
    moder &= ~(3u << (MOSI_PIN * 2));
    moder |=  (1u << (MOSI_PIN * 2));
    GPIO_MODER(GPIOB_BASE) = moder;

    /* MISO (PB11) — input (MODER = 00) */
    moder = GPIO_MODER(GPIOB_BASE);
    moder &= ~(3u << (MISO_PIN * 2));
    GPIO_MODER(GPIOB_BASE) = moder;

    /* High speed for CLK and MOSI */
    uint32_t ospeedr = GPIO_OSPEEDR(GPIOB_BASE);
    ospeedr |= (3u << (CLK_PIN * 2)) | (3u << (MOSI_PIN * 2));
    GPIO_OSPEEDR(GPIOB_BASE) = ospeedr;

    /* Bounded wait — if the chip is missing, MISO floats and STATUS_BUSY may
     * read as a stuck '1' forever. Use a short budget here (~100 ms equivalent)
     * so a dead/absent flash doesn't wedge the board at init_stage=9. The
     * watchdog would catch it eventually, but we'd loop-reboot on stage 9. */
    if (flash_wait_busy_timeout(0x80000u) != 0) {
        s_present = 0;
        return;
    }

    /* Verify chip is actually there by reading JEDEC ID. */
    uint32_t id = ext_flash_read_id();
    s_present = (id == 0xEF4016u) ? 1 : 0;
}

uint32_t ext_flash_read_id(void)
{
    cs_low();
    bb_spi_send(CMD_JEDEC_ID);
    uint32_t id = (uint32_t)bb_spi_recv() << 16;
    id |= (uint32_t)bb_spi_recv() << 8;
    id |= (uint32_t)bb_spi_recv();
    cs_high();
    return id;   /* W25Q32 → 0xEF4016 */
}

void ext_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    cs_low();
    bb_spi_send(CMD_READ_DATA);
    flash_send_addr(addr);
    for (uint32_t i = 0; i < len; i++)
        buf[i] = bb_spi_recv();
    cs_high();
}

/*
 * Program up to one page (256 bytes).
 * addr + len must not cross a 256-byte page boundary.
 */
static void flash_page_program(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    flash_write_enable();
    cs_low();
    bb_spi_send(CMD_PAGE_PROGRAM);
    flash_send_addr(addr);
    for (uint32_t i = 0; i < len; i++)
        bb_spi_send(buf[i]);
    cs_high();
    flash_wait_busy();
}

void ext_flash_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    while (len > 0) {
        /* Bytes remaining in the current 256-byte page */
        uint32_t page_remain = EXT_FLASH_PAGE_SIZE - (addr & (EXT_FLASH_PAGE_SIZE - 1));
        uint32_t chunk = (len < page_remain) ? len : page_remain;

        flash_page_program(addr, buf, chunk);

        addr += chunk;
        buf  += chunk;
        len  -= chunk;
    }
}

void ext_flash_erase_sector(uint32_t addr)
{
    flash_write_enable();
    cs_low();
    bb_spi_send(CMD_SECTOR_ERASE);
    flash_send_addr(addr);
    cs_high();
    flash_wait_busy();
}

void ext_flash_erase_block(uint32_t addr)
{
    flash_write_enable();
    cs_low();
    bb_spi_send(CMD_BLOCK_ERASE);
    flash_send_addr(addr);
    cs_high();
    flash_wait_busy();
}

void ext_flash_erase_chip(void)
{
    flash_write_enable();
    cs_low();
    bb_spi_send(CMD_CHIP_ERASE);
    cs_high();
    flash_wait_busy();
}
