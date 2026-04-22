/*
 * Bare-metal USB CDC (Virtual COM Port) for STM32U073CBT6
 *
 * Sends "Hello from STM32U073!\r\n" every second on USB serial.
 * Echoes back any received characters.
 *
 * USB pins: PA11 = USB_DM, PA12 = USB_DP (directly wired to USB connector)
 * Internal 48MHz HSI48 used for USB clock via CRS (Clock Recovery System)
 *
 * Register addresses from RM0503 and stm32u073xx.h CMSIS header.
 */

#include <stdint.h>
#include <string.h>

/* ---- Base addresses ---- */
#define RCC_BASE          0x40021000
#define PWR_BASE          0x40007000
#define GPIOA_BASE        0x50000000
#define GPIOB_BASE        0x50000400
#define USB_BASE          0x40005C00
#define USB_PMAADDR       0x40009800
#define CRS_BASE          0x40006C00
#define SYSCFG_BASE       0x40010000

/* ---- RCC registers (STM32U073 offsets from CMSIS RCC_TypeDef) ---- */
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

/* ---- CRS registers ---- */
#define CRS_CR            (*(volatile uint32_t *)(CRS_BASE + 0x00))
#define CRS_CFGR          (*(volatile uint32_t *)(CRS_BASE + 0x04))

/* ---- PWR registers ---- */
#define PWR_CR2           (*(volatile uint32_t *)(PWR_BASE + 0x04))

/* ---- GPIO registers ---- */
#define GPIOA_MODER       (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_OTYPER      (*(volatile uint32_t *)(GPIOA_BASE + 0x04))
#define GPIOA_OSPEEDR     (*(volatile uint32_t *)(GPIOA_BASE + 0x08))
#define GPIOA_AFRL        (*(volatile uint32_t *)(GPIOA_BASE + 0x20))
#define GPIOA_AFRH        (*(volatile uint32_t *)(GPIOA_BASE + 0x24))

#define GPIOB_MODER       (*(volatile uint32_t *)(GPIOB_BASE + 0x00))
#define GPIOB_BSRR        (*(volatile uint32_t *)(GPIOB_BASE + 0x18))

/* ---- USB DRD registers ---- */
#define USB_CHEP(n)       (*(volatile uint32_t *)(USB_BASE + 4*(n)))
#define USB_CNTR          (*(volatile uint32_t *)(USB_BASE + 0x40))
#define USB_ISTR          (*(volatile uint32_t *)(USB_BASE + 0x44))
#define USB_FNR           (*(volatile uint32_t *)(USB_BASE + 0x48))
#define USB_DADDR         (*(volatile uint32_t *)(USB_BASE + 0x4C))
#define USB_LPMCSR        (*(volatile uint32_t *)(USB_BASE + 0x54))
#define USB_BCDR          (*(volatile uint32_t *)(USB_BASE + 0x58))

/* ---- PMA (Packet Memory Area) access ---- */
/* On STM32U0 DRD, PMA is byte-addressable at USB_PMAADDR (1KB) */
#define PMA_BASE          USB_PMAADDR

/* Buffer descriptor table (BDT) for USB DRD:
   Each EP has 2 x 32-bit words: TXBD and RXBD
   TXBD = (tx_addr << 16) | tx_count
   RXBD = (rx_addr << 16) | rx_count_config
   BDT is always at PMA offset 0.
   8 EPs max = 8 x 8 = 64 bytes for BDT */
#define BDT_TXBD(ep)      (*(volatile uint32_t *)(PMA_BASE + 8*(ep)))
#define BDT_RXBD(ep)      (*(volatile uint32_t *)(PMA_BASE + 8*(ep) + 4))

/* PMA buffer offsets (byte addresses within PMA) */
#define PMA_BDT_SIZE      64   /* 8 EPs x 8 bytes each (we use 4) */
#define EP0_TX_BUF        64   /* EP0 TX buffer at PMA[64..127] */
#define EP0_RX_BUF        128  /* EP0 RX buffer at PMA[128..191] */
#define EP1_TX_BUF        192  /* EP1 TX (CDC data IN) at PMA[192..255] */
#define EP2_RX_BUF        256  /* EP2 RX (CDC data OUT) at PMA[256..319] */
#define EP3_TX_BUF        320  /* EP3 TX (CDC notification) at PMA[320..327] */

/* USB endpoint register bit definitions */
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

/* Endpoint types */
#define USB_EP_BULK       (0 << 9)
#define USB_EP_CONTROL    (1 << 9)
#define USB_EP_ISOCHRONOUS (2 << 9)
#define USB_EP_INTERRUPT  (3 << 9)

/* Stat values */
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
#define USB_CNTR_L1REQM   (1 << 7)
#define USB_CNTR_L1XACT   (1 << 6)
#define USB_CNTR_L1RES    (1 << 5)
#define USB_CNTR_L2RES    (1 << 4)
#define USB_CNTR_FRES     (1 << 1)
#define USB_CNTR_USBRST   (1 << 0)

/* ISTR bits */
#define USB_ISTR_CTR      (1 << 15)
#define USB_ISTR_PMAOVR   (1 << 14)
#define USB_ISTR_ERR      (1 << 13)
#define USB_ISTR_WKUP     (1 << 12)
#define USB_ISTR_SUSP     (1 << 11)
#define USB_ISTR_RESET    (1 << 10)
#define USB_ISTR_SOF      (1 << 9)
#define USB_ISTR_ESOF     (1 << 8)
#define USB_ISTR_L1REQ    (1 << 7)
#define USB_ISTR_DIR      (1 << 4)
#define USB_ISTR_EP_ID    (0xF)

/* BCDR bits */
#define USB_BCDR_DPPU     (1 << 15)

/* RXBD count field encoding (in UPPER bits of RXBD word per DRD format):
   Bit 31: BL_SIZE
   Bits 30:26: NUM_BLOCK
   Bits 25:16: RX byte count (set by hardware)
   Bits 15:0: RX buffer address (byte offset, 4-byte aligned)
   For buffers > 62 bytes: BL_SIZE=1, NUM_BLOCK = (size/32)-1 */
#define PMA_RXBD_BL_SIZE  (1U << 31)
#define PMA_RXBD_64       (PMA_RXBD_BL_SIZE | (1U << 26))  /* BL_SIZE=1, NUM_BLOCK=1 => 64 bytes */

/* ---- USB Descriptors ---- */

/* Device descriptor */
static const uint8_t device_desc[] = {
    18,         /* bLength */
    1,          /* bDescriptorType: DEVICE */
    0x00, 0x02, /* bcdUSB: 2.00 */
    0x02,       /* bDeviceClass: CDC */
    0x02,       /* bDeviceSubClass: ACM */
    0x00,       /* bDeviceProtocol */
    64,         /* bMaxPacketSize0 */
    0x83, 0x04, /* idVendor: 0x0483 (ST) */
    0x40, 0x57, /* idProduct: 0x5740 (Virtual COM Port) */
    0x00, 0x02, /* bcdDevice: 2.00 */
    1,          /* iManufacturer */
    2,          /* iProduct */
    3,          /* iSerialNumber */
    1,          /* bNumConfigurations */
};

/* Configuration descriptor + interface + CDC descriptors + endpoints */
static const uint8_t config_desc[] = {
    /* Configuration descriptor */
    9, 2,               /* bLength, bDescriptorType */
    67, 0,              /* wTotalLength: 67 bytes */
    2,                  /* bNumInterfaces */
    1,                  /* bConfigurationValue */
    0,                  /* iConfiguration */
    0x80,               /* bmAttributes: bus powered */
    50,                 /* bMaxPower: 100mA */

    /* Interface 0: CDC Communication */
    9, 4,               /* bLength, bDescriptorType */
    0,                  /* bInterfaceNumber */
    0,                  /* bAlternateSetting */
    1,                  /* bNumEndpoints */
    0x02,               /* bInterfaceClass: CDC */
    0x02,               /* bInterfaceSubClass: ACM */
    0x01,               /* bInterfaceProtocol: AT commands */
    0,                  /* iInterface */

    /* CDC Header Functional Descriptor */
    5, 0x24, 0x00, 0x10, 0x01,

    /* CDC Call Management */
    5, 0x24, 0x01, 0x00, 0x01,

    /* CDC ACM Functional Descriptor */
    4, 0x24, 0x02, 0x02,

    /* CDC Union */
    5, 0x24, 0x06, 0x00, 0x01,

    /* Endpoint 3 IN: Notification */
    7, 5,
    0x83,               /* bEndpointAddress: EP3 IN */
    0x03,               /* bmAttributes: Interrupt */
    8, 0,               /* wMaxPacketSize: 8 */
    255,                /* bInterval */

    /* Interface 1: CDC Data */
    9, 4,
    1,                  /* bInterfaceNumber */
    0,                  /* bAlternateSetting */
    2,                  /* bNumEndpoints */
    0x0A,               /* bInterfaceClass: CDC Data */
    0, 0,               /* SubClass, Protocol */
    0,                  /* iInterface */

    /* Endpoint 1 IN: Data */
    7, 5,
    0x81,               /* bEndpointAddress: EP1 IN */
    0x02,               /* bmAttributes: Bulk */
    64, 0,              /* wMaxPacketSize: 64 */
    0,                  /* bInterval */

    /* Endpoint 2 OUT: Data */
    7, 5,
    0x02,               /* bEndpointAddress: EP2 OUT */
    0x02,               /* bmAttributes: Bulk */
    64, 0,              /* wMaxPacketSize: 64 */
    0,                  /* bInterval */
};

/* String descriptor 0: Language */
static const uint8_t string0_desc[] = { 4, 3, 0x09, 0x04 };

/* String descriptor helper macro - UTF-16LE */
#define USB_STR_DESC(s) \
    static const uint8_t s[]

/* Manufacturer string */
static const uint8_t string1_desc[] = {
    14, 3,
    'S',0, 'a',0, 't',0, 'u',0, 'r',0, 'n',0,
};

/* Product string */
static const uint8_t string2_desc[] = {
    38, 3,
    'S',0, 'T',0, 'M',0, '3',0, '2',0, ' ',0,
    'L',0, 'o',0, 'R',0, 'a',0, ' ',0,
    'T',0, 'r',0, 'a',0, 'c',0, 'k',0, 'e',0, 'r',0,
};

/* Serial string */
static const uint8_t string3_desc[] = {
    10, 3,
    '0',0, '0',0, '0',0, '1',0,
};

/* ---- State variables ---- */
static volatile uint8_t usb_address = 0;
static volatile uint8_t usb_set_address_pending = 0;
static volatile uint8_t usb_configured = 0;
static volatile uint8_t ep1_tx_busy = 0;

/* CDC line coding (115200 8N1) */
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08 };

/* ---- PMA access helpers ---- */

/* Write data to PMA. On STM32U0 DRD, PMA is byte-addressable
   but must be accessed as 32-bit words (4 bytes per word). */
static void pma_write(uint16_t pma_offset, const uint8_t *buf, uint16_t len)
{
    /* PMA is 32-bit word accessible with all 4 bytes valid */
    volatile uint32_t *dst;
    /* For simplicity, assume pma_offset is 4-byte aligned */
    dst = (volatile uint32_t *)(PMA_BASE + pma_offset);
    uint16_t i;
    for (i = 0; i + 3 < len; i += 4) {
        *dst++ = (uint32_t)buf[i] | ((uint32_t)buf[i+1] << 8) |
                 ((uint32_t)buf[i+2] << 16) | ((uint32_t)buf[i+3] << 24);
    }
    if (i < len) {
        uint32_t val = 0;
        for (uint16_t j = 0; j < (len - i); j++) {
            val |= ((uint32_t)buf[i+j]) << (j * 8);
        }
        *dst = val;
    }
}

/* Read data from PMA */
static void pma_read(uint16_t pma_offset, uint8_t *buf, uint16_t len)
{
    volatile uint32_t *src = (volatile uint32_t *)(PMA_BASE + pma_offset);
    uint16_t i;
    for (i = 0; i + 3 < len; i += 4) {
        uint32_t val = *src++;
        buf[i]   = val & 0xFF;
        buf[i+1] = (val >> 8) & 0xFF;
        buf[i+2] = (val >> 16) & 0xFF;
        buf[i+3] = (val >> 24) & 0xFF;
    }
    if (i < len) {
        uint32_t val = *src;
        for (uint16_t j = 0; j < (len - i); j++) {
            buf[i+j] = (val >> (j * 8)) & 0xFF;
        }
    }
}

/* ---- BDT (Buffer Descriptor Table) helpers for USB DRD ---- */
/* BDT is at PMA offset 0. Each EP has 2 x 32-bit words = 8 bytes:
   TXBD = (tx_addr << 16) | tx_count
   RXBD = (rx_addr << 16) | rx_count_config */

static void bdt_set_tx(uint8_t ep, uint16_t addr, uint16_t count)
{
    /* TXBD format: count in bits [31:16], addr in bits [15:0] */
    BDT_TXBD(ep) = ((uint32_t)count << 16) | (addr & ~3U);
}

static void bdt_set_tx_count(uint8_t ep, uint16_t count)
{
    /* Update only the count field (upper 16 bits), keep address */
    uint32_t val = BDT_TXBD(ep);
    BDT_TXBD(ep) = (val & 0x0000FFFFUL) | ((uint32_t)count << 16);
}

static void bdt_set_rx(uint8_t ep, uint16_t addr, uint32_t count_config)
{
    /* RXBD format: BL_SIZE|NUM_BLOCK|COUNT in bits [31:16], addr in bits [15:0] */
    BDT_RXBD(ep) = count_config | (addr & ~3U);
}

static uint16_t bdt_get_rx_count(uint8_t ep)
{
    return (BDT_RXBD(ep) >> 16) & 0x3FF;
}

/* Restore RXBD count config (BL_SIZE + NUM_BLOCK) after hardware modifies it.
   The USB DRD hardware overwrites the upper bits of RXBD when writing COUNT_RX,
   so we must restore BL_SIZE and NUM_BLOCK before re-arming reception. */
static void bdt_restore_rx_config(uint8_t ep, uint32_t count_config)
{
    uint32_t val = BDT_RXBD(ep);
    BDT_RXBD(ep) = (val & 0x0000FFFFU) | count_config;
}

/* Forward declaration */
static void ep_set_rx_status(uint8_t ep, uint32_t status);

/* Re-arm EP0 RX: restore RXBD config then set VALID */
static void ep0_arm_rx(void)
{
    /* Debug: write canary values to EP0 RX PMA before arming */
    *(volatile uint32_t *)(PMA_BASE + EP0_RX_BUF) = 0xDEADBEEF;
    *(volatile uint32_t *)(PMA_BASE + EP0_RX_BUF + 4) = 0xCAFEBABE;
    bdt_restore_rx_config(0, PMA_RXBD_64);
    ep_set_rx_status(0, USB_EP_RX_VALID);
}

/* Re-arm EP2 RX: restore RXBD config then set VALID */
static void ep2_arm_rx(void)
{
    bdt_restore_rx_config(2, PMA_RXBD_64);
    ep_set_rx_status(2, USB_EP_RX_VALID);
}

/* ---- EP register manipulation helpers ---- */
/* USB EP registers have toggle bits and write-0-to-clear bits, so we need
   special care when writing them. */

/* USB DRD CHEP register manipulation.
   The 32-bit CHEP register has:
   - RW bits (written as-is): UTYPE[9:10], KIND[8], ADDR[3:0] + upper bits
   - Toggle bits (XOR to change): STAT_TX[5:4], STAT_RX[13:12], DTOG_TX[6], DTOG_RX[14]
   - Write-0-to-clear bits: VTTX[7], VTRX[15]
   We must preserve the upper 16 bits (DEVADDR, NAK, LSEP, ERR*, etc.) as 0
   since writing 1 to toggle bits or error bits could cause issues.
   USB_CHEP_REG_MASK from CMSIS = 0x07FF8F8F */
#define CHEP_REG_MASK     0x07FF8F8FUL  /* All non-toggle, non-clear bits */
/* RW bits in lower 16 bits only */
#define EP_RW_BITS        (USB_EP_TYPE | USB_EP_KIND | USB_EP_EA)  /* 0x070F */

static void ep_set_tx_status(uint8_t ep, uint32_t status)
{
    uint32_t val = USB_CHEP(ep);
    /* For toggle bits: writing 1 toggles, writing 0 keeps.
       To set STAT_TX to desired value, XOR current with desired.
       For STAT_RX, DTOG_RX, DTOG_TX: write 0 to not change them.
       For VTRX/VTTX (w0c): write 1 to keep, write 0 to clear. */
    uint32_t w = (val & EP_RW_BITS)  /* Keep type, kind, addr */
                 | USB_EP_CTR_RX | USB_EP_CTR_TX  /* Don't clear either CTR */
                 | ((val ^ status) & USB_EP_STAT_TX);  /* Toggle TX stat to target */
    USB_CHEP(ep) = w;
}

static void ep_set_rx_status(uint8_t ep, uint32_t status)
{
    uint32_t val = USB_CHEP(ep);
    uint32_t w = (val & EP_RW_BITS)
                 | USB_EP_CTR_RX | USB_EP_CTR_TX
                 | ((val ^ status) & USB_EP_STAT_RX);
    USB_CHEP(ep) = w;
}

static void ep_clear_ctr_rx(uint8_t ep)
{
    uint32_t val = USB_CHEP(ep);
    /* Write 0 to VTRX (bit 15) to clear it, write 1 to VTTX to keep it.
       Write 0 to all toggle bits (STAT_TX, STAT_RX, DTOG_TX, DTOG_RX)
       so they are NOT changed. */
    uint32_t w = (val & EP_RW_BITS)
                 | USB_EP_CTR_TX;  /* Keep VTTX=1, VTRX=0 to clear */
    USB_CHEP(ep) = w;
}

static void ep_clear_ctr_tx(uint8_t ep)
{
    uint32_t val = USB_CHEP(ep);
    uint32_t w = (val & EP_RW_BITS)
                 | USB_EP_CTR_RX;  /* Keep VTRX=1, VTTX=0 to clear */
    USB_CHEP(ep) = w;
}

/* ---- EP0 transmit ---- */
static void ep0_tx(const uint8_t *data, uint16_t len)
{
    if (len > 64) len = 64;
    pma_write(EP0_TX_BUF, data, len);
    bdt_set_tx_count(0, len);
    ep_set_tx_status(0, USB_EP_TX_VALID);
}

static void ep0_tx_zlp(void)
{
    bdt_set_tx_count(0, 0);
    ep_set_tx_status(0, USB_EP_TX_VALID);
}

/* ---- EP1 (CDC data IN) transmit ---- */
static void ep1_tx(const uint8_t *data, uint16_t len)
{
    if (len > 64) len = 64;
    pma_write(EP1_TX_BUF, data, len);
    bdt_set_tx_count(1, len);
    ep1_tx_busy = 1;
    ep_set_tx_status(1, USB_EP_TX_VALID);
}

/* ---- USB Reset handler ---- */
static void usb_reset(void)
{
    /* BDT is always at PMA[0] on STM32U0 DRD */

    /* EP0: Control, 64 byte TX and RX */
    bdt_set_tx(0, EP0_TX_BUF, 0);
    bdt_set_rx(0, EP0_RX_BUF, PMA_RXBD_64);

    /* EP1: Bulk IN (CDC Data TX) */
    bdt_set_tx(1, EP1_TX_BUF, 0);
    bdt_set_rx(1, 0, 0);  /* EP1 has no RX */

    /* EP2: Bulk OUT (CDC Data RX) */
    bdt_set_tx(2, 0, 0);  /* EP2 has no TX */
    bdt_set_rx(2, EP2_RX_BUF, PMA_RXBD_64);

    /* EP3: Interrupt IN (CDC Notification) */
    bdt_set_tx(3, EP3_TX_BUF, 0);
    bdt_set_rx(3, 0, 0);  /* EP3 has no RX */

    /* Configure EP0: Control.
       STAT and DTOG are toggle-on-write bits. To set them to a known value,
       first toggle them to 0 (by writing back their current value), then
       write the desired value (which XORs with 0 = identity). */
    {
        uint32_t val = USB_CHEP(0);
        uint32_t toggles = val & (USB_EP_STAT_TX | USB_EP_STAT_RX | USB_EP_DTOG_TX | USB_EP_DTOG_RX);
        if (toggles) {
            /* Reset all toggle bits to 0 */
            USB_CHEP(0) = (val & EP_RW_BITS) | USB_EP_CTR_RX | USB_EP_CTR_TX | toggles;
        }
    }
    /* Now all toggle bits are 0; direct write sets desired values exactly */
    USB_CHEP(0) = USB_EP_CONTROL | USB_EP_RX_VALID | USB_EP_TX_NAK | 0;
    /* Debug: store EP0 readback at safe RAM location */
    *(volatile uint32_t *)0x20008000 = USB_CHEP(0);
    *(volatile uint32_t *)0x20008004 = BDT_TXBD(0);
    *(volatile uint32_t *)0x20008008 = BDT_RXBD(0);
    (*(volatile uint32_t *)0x2000800C)++;

    /* Configure EP1: Bulk IN - reset toggles first */
    {
        uint32_t val = USB_CHEP(1);
        uint32_t toggles = val & (USB_EP_STAT_TX | USB_EP_STAT_RX | USB_EP_DTOG_TX | USB_EP_DTOG_RX);
        if (toggles)
            USB_CHEP(1) = (val & EP_RW_BITS) | USB_EP_CTR_RX | USB_EP_CTR_TX | toggles;
    }
    USB_CHEP(1) = USB_EP_BULK | USB_EP_TX_NAK | 1;

    /* Configure EP2: Bulk OUT - reset toggles first */
    {
        uint32_t val = USB_CHEP(2);
        uint32_t toggles = val & (USB_EP_STAT_TX | USB_EP_STAT_RX | USB_EP_DTOG_TX | USB_EP_DTOG_RX);
        if (toggles)
            USB_CHEP(2) = (val & EP_RW_BITS) | USB_EP_CTR_RX | USB_EP_CTR_TX | toggles;
    }
    USB_CHEP(2) = USB_EP_BULK | USB_EP_RX_VALID | 2;

    /* Configure EP3: Interrupt IN - reset toggles first */
    {
        uint32_t val = USB_CHEP(3);
        uint32_t toggles = val & (USB_EP_STAT_TX | USB_EP_STAT_RX | USB_EP_DTOG_TX | USB_EP_DTOG_RX);
        if (toggles)
            USB_CHEP(3) = (val & EP_RW_BITS) | USB_EP_CTR_RX | USB_EP_CTR_TX | toggles;
    }
    USB_CHEP(3) = USB_EP_INTERRUPT | USB_EP_TX_NAK | 3;

    /* Enable device, address 0 */
    USB_DADDR = 0x80; /* EF bit set, address = 0 */

    usb_address = 0;
    usb_set_address_pending = 0;
    usb_configured = 0;
    ep1_tx_busy = 0;
}

/* ---- Setup packet handler ---- */
static uint8_t setup_buf[8];
static const uint8_t *tx_ptr;
static uint16_t tx_remaining;

static void handle_setup(void)
{
    pma_read(EP0_RX_BUF, setup_buf, 8);

    /* Debug: store SETUP packet at 0x20009F20 */
    *(volatile uint32_t *)0x20008020 = setup_buf[0] | (setup_buf[1]<<8) | (setup_buf[2]<<16) | (setup_buf[3]<<24);
    *(volatile uint32_t *)0x20008024 = setup_buf[4] | (setup_buf[5]<<8) | (setup_buf[6]<<16) | (setup_buf[7]<<24);
    (*(volatile uint32_t *)0x20008028)++;  /* setup count */

    uint8_t bmRequestType = setup_buf[0];
    uint8_t bRequest = setup_buf[1];
    uint16_t wValue = setup_buf[2] | (setup_buf[3] << 8);
    uint16_t wLength = setup_buf[6] | (setup_buf[7] << 8);

    tx_ptr = 0;
    tx_remaining = 0;

    if ((bmRequestType & 0x60) == 0x00) {
        /* Standard requests */
        switch (bRequest) {
        case 0x06: /* GET_DESCRIPTOR */
        {
            uint8_t desc_type = wValue >> 8;
            uint8_t desc_index = wValue & 0xFF;
            const uint8_t *desc = 0;
            uint16_t desc_len = 0;

            switch (desc_type) {
            case 1: /* DEVICE */
                desc = device_desc;
                desc_len = sizeof(device_desc);
                break;
            case 2: /* CONFIGURATION */
                desc = config_desc;
                desc_len = sizeof(config_desc);
                break;
            case 3: /* STRING */
                switch (desc_index) {
                case 0: desc = string0_desc; desc_len = string0_desc[0]; break;
                case 1: desc = string1_desc; desc_len = string1_desc[0]; break;
                case 2: desc = string2_desc; desc_len = string2_desc[0]; break;
                case 3: desc = string3_desc; desc_len = string3_desc[0]; break;
                }
                break;
            case 6: /* DEVICE_QUALIFIER - not supported in FS, stall */
                ep_set_tx_status(0, USB_EP_TX_STALL);
                ep0_arm_rx();
                return;
            }

            if (desc) {
                if (wLength > 0 && desc_len > wLength) desc_len = wLength;
                uint16_t chunk = desc_len > 64 ? 64 : desc_len;
                ep0_tx(desc, chunk);
                tx_ptr = desc + chunk;
                tx_remaining = desc_len - chunk;
                ep0_arm_rx();
                return;
            }
            /* Unknown descriptor - stall */
            ep_set_tx_status(0, USB_EP_TX_STALL);
            ep0_arm_rx();
            return;
        }

        case 0x05: /* SET_ADDRESS */
            usb_address = wValue & 0x7F;
            usb_set_address_pending = 1;
            ep0_tx_zlp();
            ep0_arm_rx();
            return;

        case 0x09: /* SET_CONFIGURATION */
            usb_configured = (wValue != 0);
            ep0_tx_zlp();
            ep0_arm_rx();
            return;

        case 0x00: /* GET_STATUS */
        {
            static const uint8_t status[2] = { 0, 0 };
            ep0_tx(status, 2);
            ep0_arm_rx();
            return;
        }

        case 0x01: /* CLEAR_FEATURE */
        case 0x03: /* SET_FEATURE */
            ep0_tx_zlp();
            ep0_arm_rx();
            return;
        }
    }
    else if ((bmRequestType & 0x60) == 0x20) {
        /* Class requests (CDC) */
        switch (bRequest) {
        case 0x20: /* SET_LINE_CODING */
            /* Host will send 7 bytes in DATA phase, we need to receive them */
            ep0_arm_rx();
            /* We'll ACK after receiving the data */
            return;

        case 0x21: /* GET_LINE_CODING */
            ep0_tx(line_coding, 7);
            ep0_arm_rx();
            return;

        case 0x22: /* SET_CONTROL_LINE_STATE */
            ep0_tx_zlp();
            ep0_arm_rx();
            return;
        }
    }

    /* Unhandled - stall */
    ep_set_tx_status(0, USB_EP_TX_STALL);
    ep_set_rx_status(0, USB_EP_RX_STALL);
}

/* ---- USB polling ---- */
static void usb_poll(void)
{
    uint32_t istr = USB_ISTR;

    /* Debug: store first non-zero ISTR and count of poll calls */
    (*(volatile uint32_t *)0x20008010)++;  /* poll count */
    if (istr && !(*(volatile uint32_t *)0x20008014)) {
        *(volatile uint32_t *)0x20008014 = istr;  /* first non-zero ISTR */
    }
    if (istr & USB_ISTR_RESET) {
        *(volatile uint32_t *)0x20008018 = istr;  /* ISTR at reset time */
    }

    /* Reset */
    if (istr & USB_ISTR_RESET) {
        USB_ISTR = ~USB_ISTR_RESET;
        usb_reset();
        return;
    }

    /* Correct transfer */
    if (istr & USB_ISTR_CTR) {
        uint8_t ep_num = istr & USB_ISTR_EP_ID;
        uint32_t ep_val = USB_CHEP(ep_num);

        /* Debug: CTR event info */
        (*(volatile uint32_t *)0x2000802C)++;  /* CTR count */
        *(volatile uint32_t *)0x20008030 = ep_val;  /* last EP val at CTR */
        *(volatile uint32_t *)0x20008034 = istr;    /* last ISTR at CTR */

        if (ep_num == 0) {
            /* EP0 */
            if (ep_val & USB_EP_CTR_RX) {
                if (ep_val & USB_EP_SETUP) {
                    /* WA: few cycles for RX PMA descriptor to update
                       (same workaround as HAL PCD_RX_PMA_CNT) */
                    for (volatile int i = 0; i < 20; i++) __asm__("nop");
                    /* Read SETUP data BEFORE clearing CTR_RX
                       (PMA is frozen while CTR_RX=1) */
                    /* Debug: RXBD count (set by hardware), raw PMA words */
                    *(volatile uint32_t *)0x20008038 = BDT_RXBD(0); /* RXBD with HW count */
                    *(volatile uint32_t *)0x2000803C = *(volatile uint32_t *)(PMA_BASE + EP0_RX_BUF + 4);
                    ep_clear_ctr_rx(0);
                    handle_setup();
                } else {
                    ep_clear_ctr_rx(0);
                    /* EP0 OUT data - could be SET_LINE_CODING data */
                    uint16_t count = bdt_get_rx_count(0);
                    if (count == 7) {
                        pma_read(EP0_RX_BUF, line_coding, 7);
                    }
                    ep0_tx_zlp();
                    ep0_arm_rx();
                }
            }
            if (ep_val & USB_EP_CTR_TX) {
                ep_clear_ctr_tx(0);
                /* TX complete on EP0 */
                if (usb_set_address_pending) {
                    USB_DADDR = 0x80 | usb_address;
                    usb_set_address_pending = 0;
                }
                /* Continue multi-packet transfer */
                if (tx_remaining > 0 && tx_ptr) {
                    uint16_t chunk = tx_remaining > 64 ? 64 : tx_remaining;
                    ep0_tx(tx_ptr, chunk);
                    tx_ptr += chunk;
                    tx_remaining -= chunk;
                }
                ep0_arm_rx();
            }
        }
        else if (ep_num == 1) {
            /* EP1 IN complete (CDC Data TX done) */
            if (ep_val & USB_EP_CTR_TX) {
                ep_clear_ctr_tx(1);
                ep1_tx_busy = 0;
            }
        }
        else if (ep_num == 2) {
            /* EP2 OUT (CDC Data RX) */
            if (ep_val & USB_EP_CTR_RX) {
                ep_clear_ctr_rx(2);
                uint16_t count = bdt_get_rx_count(2);
                if (count > 0 && count <= 64) {
                    uint8_t rxbuf[64];
                    pma_read(EP2_RX_BUF, rxbuf, count);
                    /* Echo back received data */
                    if (!ep1_tx_busy) {
                        ep1_tx(rxbuf, count);
                    }
                }
                ep2_arm_rx();
            }
        }
        else if (ep_num == 3) {
            /* EP3 notification TX complete */
            if (ep_val & USB_EP_CTR_TX) {
                ep_clear_ctr_tx(3);
            }
        }
    }

    /* Clear other interrupt flags */
    if (istr & USB_ISTR_SUSP) {
        USB_ISTR = ~USB_ISTR_SUSP;
        USB_CNTR |= USB_CNTR_SUSPM;
    }
    if (istr & USB_ISTR_WKUP) {
        USB_ISTR = ~USB_ISTR_WKUP;
    }
    if (istr & USB_ISTR_SOF) {
        USB_ISTR = ~USB_ISTR_SOF;
    }
    if (istr & USB_ISTR_ERR) {
        USB_ISTR = ~USB_ISTR_ERR;
    }
    if (istr & USB_ISTR_PMAOVR) {
        USB_ISTR = ~USB_ISTR_PMAOVR;
    }
    if (istr & USB_ISTR_ESOF) {
        USB_ISTR = ~USB_ISTR_ESOF;
    }
}

/* ---- Delay ---- */
static void delay_ms(uint32_t ms)
{
    volatile uint32_t count = ms * 4000;  /* ~16MHz HSI16 */
    while (count--) __asm__ volatile ("nop");
}

/* ---- Clock init: enable HSI16 as sysclk, HSI48 + CRS for USB ---- */
static void clock_init(void)
{
    volatile uint32_t *bc = (volatile uint32_t *)0x20000004;

    /* Enable PWR clock */
    *bc = 0x30;
    RCC_APBENR1 |= (1 << 28);  /* PWREN */
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    /* Enable HSI16 and switch system clock to 16MHz */
    *bc = 0x30a;
    RCC_CR |= (1 << 8);  /* HSI16ON */
    while (!(RCC_CR & (1 << 10)));  /* Wait HSI16RDY */
    /* Set flash wait state for 16MHz (WS=0 is ok up to 16MHz on STM32U0) */
    RCC_CFGR = (RCC_CFGR & ~7U) | 1U;  /* SW = 001 = HSI16 */
    while ((RCC_CFGR & (7U << 3)) != (1U << 3));  /* Wait SWS = HSI16 */

    /* Enable HSI48 */
    *bc = 0x31;
    RCC_CRRCR |= (1 << 0);  /* HSI48ON */
    *bc = 0x32;
    while (!(RCC_CRRCR & (1 << 1)));  /* Wait HSI48RDY */

    /* Select HSI48 as USB clock: CCIPR CLK48SEL = 11 (HSI48) */
    *bc = 0x33;
    RCC_CCIPR = (RCC_CCIPR & ~(3 << 26)) | (3 << 26);  /* CLK48SEL = 11 = HSI48 */

    /* Enable CRS clock */
    *bc = 0x34;
    RCC_APBENR1 |= (1 << 16);  /* CRSEN */
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    /* Configure CRS to sync HSI48 from USB SOF */
    *bc = 0x35;
    CRS_CFGR = (CRS_CFGR & ~(3 << 28)) | (2 << 28);  /* SYNCSRC = 10 (USB SOF) */
    *bc = 0x36;
    /* Enable auto-trimming and frequency error counter */
    CRS_CR |= (1 << 5) | (1 << 6);  /* AUTOTRIMEN | CEN */
    *bc = 0x37;
}

/* ---- USB peripheral init ---- */
static void usb_init(void)
{
    /* Enable GPIOA clock for USB pins */
    RCC_IOPENR |= (1 << 0);  /* GPIOAEN */
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    /* PA11 (USB_DM) and PA12 (USB_DP) must be set to AF10 for USB DRD.
       MODER = 10 (alternate function), AFRH AF10 for pins 11 and 12. */
    {
        uint32_t moder = GPIOA_MODER;
        moder &= ~((3 << 22) | (3 << 24));  /* Clear PA11, PA12 */
        moder |=  ((2 << 22) | (2 << 24));  /* AF mode for PA11, PA12 */
        GPIOA_MODER = moder;

        /* AFRH: PA11 = AF10 (bits 15:12), PA12 = AF10 (bits 19:16) */
        uint32_t afrh = GPIOA_AFRH;
        afrh &= ~((0xF << 12) | (0xF << 16));
        afrh |=  ((10 << 12) | (10 << 16));  /* AF10 for both */
        GPIOA_AFRH = afrh;

        /* Set high speed for USB pins */
        uint32_t ospeedr = GPIOA_OSPEEDR;
        ospeedr |= (3 << 22) | (3 << 24);  /* Very high speed */
        GPIOA_OSPEEDR = ospeedr;
    }

    /* Enable USB clock */
    RCC_APBENR1 |= (1 << 13);  /* USBEN */
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    /* Validate USB supply voltage */
    PWR_CR2 |= (1 << 10);  /* USV - USB Supply Valid */

    /* USB DRD init sequence (RM0503):
       After clock enable, CNTR defaults to PDWN=1, USBRST=1 (0x03).
       1. Clear PDWN (bit 1) while keeping USBRST (bit 0) asserted
       2. Wait ~1us for analog startup
       3. Clear USBRST to take peripheral out of reset
       4. Clear ISTR
       5. Set interrupt enables */

    /* Step 1: Exit power-down, keep USB reset asserted */
    USB_CNTR = (1 << 0);  /* USBRST=1, PDWN=0 */
    delay_ms(1);  /* Wait for analog startup (needs ~1us, 1ms is safe) */

    /* Step 2: Clear USB reset */
    USB_CNTR = 0;
    delay_ms(1);

    /* Zero entire PMA data area (offset 0 to 1020) for diagnostics.
       BDT will be written later by usb_reset(). */
    for (int i = 0; i < 1024; i += 4)
        *(volatile uint32_t *)(PMA_BASE + i) = 0;

    /* Clear any pending interrupts */
    USB_ISTR = 0;

    /* Enable pullup on DP to signal device connection */
    USB_BCDR |= USB_BCDR_DPPU;

    /* Enable USB interrupts we care about */
    USB_CNTR = USB_CNTR_CTRM | USB_CNTR_RESETM | USB_CNTR_SUSPM | USB_CNTR_WKUPM | USB_CNTR_ERRM | USB_CNTR_ESOFM | USB_CNTR_SOFM;

    /* Enable function - required for the peripheral to detect bus resets */
    USB_DADDR = 0x80;  /* EF=1, address=0 */
}

/* ---- CDC transmit string ---- */
static void cdc_print(const char *str)
{
    if (!usb_configured) return;

    uint16_t len = 0;
    while (str[len]) len++;
    if (len == 0) return;

    /* Wait for EP1 to be free (with timeout) */
    for (volatile int t = 0; t < 100000 && ep1_tx_busy; t++) {
        usb_poll();
    }

    if (!ep1_tx_busy) {
        ep1_tx((const uint8_t *)str, len > 64 ? 64 : len);
    }
}

/* ---- LED helpers (PB13, PB14) ---- */
static void led_init(void)
{
    RCC_IOPENR |= (1 << 1);  /* GPIOBEN */
    for (volatile int i = 0; i < 10; i++) __asm__("nop");
    uint32_t moder = GPIOB_MODER;
    moder &= ~((3 << 26) | (3 << 28));
    moder |=  ((1 << 26) | (1 << 28));
    GPIOB_MODER = moder;
}

static inline void led1_on(void)  { GPIOB_BSRR = (1 << 13); }
static inline void led1_off(void) { GPIOB_BSRR = (1 << (13 + 16)); }
static inline void led2_toggle(void)
{
    static uint8_t state = 0;
    if (state) { GPIOB_BSRR = (1 << (14 + 16)); state = 0; }
    else       { GPIOB_BSRR = (1 << 14); state = 1; }
}

/* ---- HardFault handler - fast blink both LEDs ---- */
void HardFault_Handler(void)
{
    /* Enable GPIOB if not already */
    RCC_IOPENR |= (1 << 1);
    for (volatile int i = 0; i < 10; i++) __asm__("nop");
    uint32_t moder = GPIOB_MODER;
    moder &= ~((3 << 26) | (3 << 28));
    moder |=  ((1 << 26) | (1 << 28));
    GPIOB_MODER = moder;

    while (1) {
        GPIOB_BSRR = (1 << 13) | (1 << 14);
        for (volatile int i = 0; i < 100000; i++) __asm__("nop");
        GPIOB_BSRR = (1 << (13+16)) | (1 << (14+16));
        for (volatile int i = 0; i < 100000; i++) __asm__("nop");
    }
}

/* ---- Main entry point ---- */
void Reset_Handler(void)
{
    /* Debug breadcrumbs at 0x20000000 */
    #define BREADCRUMB (*(volatile uint32_t *)0x20000000)
    BREADCRUMB = 0x01;

    /* Clear debug area (at 0x20008000, safe from stack at top of RAM) */
    for (int i = 0; i < 16; i++)
        ((volatile uint32_t *)0x20008000)[i] = 0;

    led_init();

    BREADCRUMB = 0x02;
    led1_on();  /* LED1 on = MCU alive */

    BREADCRUMB = 0x03;
    clock_init();

    BREADCRUMB = 0x04;
    usb_init();

    uint32_t counter = 0;
    uint32_t tick = 0;

    while (1) {
        usb_poll();

        tick++;
        if (tick >= 200000) {
            tick = 0;
            counter++;
            led2_toggle();

            /* Print message every ~1 second */
            if (usb_configured) {
                char buf[64];
                /* Simple integer to string */
                char *p = buf;
                const char *msg = "Hello from STM32U073! count=";
                while (*msg) *p++ = *msg++;

                /* Convert counter to decimal */
                char num[10];
                int ni = 0;
                uint32_t c = counter;
                if (c == 0) num[ni++] = '0';
                else {
                    while (c > 0) { num[ni++] = '0' + (c % 10); c /= 10; }
                }
                for (int i = ni - 1; i >= 0; i--) *p++ = num[i];
                *p++ = '\r';
                *p++ = '\n';
                *p = 0;
                cdc_print(buf);
            }
        }
    }
}

/* ---- Vector table ---- */
extern uint32_t _estack;

__attribute__((section(".isr_vector")))
const uint32_t vectors[] = {
    (uint32_t)&_estack,
    (uint32_t)Reset_Handler,
    (uint32_t)HardFault_Handler,  /* NMI */
    (uint32_t)HardFault_Handler,  /* HardFault */
};
