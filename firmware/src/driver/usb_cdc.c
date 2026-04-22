/*
 * usb_cdc.c — USB CDC driver for STM32U073 (bare-metal)
 *
 * Extracted from the working usb_cdc/main.c test firmware.
 * Provides: usb_cdc_init(), usb_cdc_poll(), cdc_print_str()
 * and a receive callback for incoming terminal data.
 */

#include "usb_cdc.h"
#include "stm32u0.h"
#include <string.h>

/* ---- State ---- */
static volatile uint8_t usb_address = 0;
static volatile uint8_t usb_set_address_pending = 0;
static volatile uint8_t usb_configured = 0;
static volatile uint8_t ep1_tx_busy = 0;
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08 };
static cdc_rx_callback_t rx_callback = 0;

/* ---- Descriptors ---- */
static const uint8_t device_desc[] = {
    18, 1, 0x00, 0x02, 0x02, 0x02, 0x00, 64,
    0x83, 0x04, 0x40, 0x57, 0x00, 0x02, 1, 2, 3, 1,
};

static const uint8_t config_desc[] = {
    9, 2, 67, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, 0x10, 0x01,
    5, 0x24, 0x01, 0x00, 0x01,
    4, 0x24, 0x02, 0x02,
    5, 0x24, 0x06, 0x00, 0x01,
    7, 5, 0x83, 0x03, 8, 0, 255,
    9, 4, 1, 0, 2, 0x0A, 0, 0, 0,
    7, 5, 0x81, 0x02, 64, 0, 0,
    7, 5, 0x02, 0x02, 64, 0, 0,
};

static const uint8_t string0_desc[] = { 4, 3, 0x09, 0x04 };
static const uint8_t string1_desc[] = { 14, 3, 'S',0,'a',0,'t',0,'u',0,'r',0,'n',0 };
static const uint8_t string2_desc[] = {
    38, 3, 'S',0,'T',0,'M',0,'3',0,'2',0,' ',0,
    'L',0,'o',0,'R',0,'a',0,' ',0,'T',0,'r',0,'a',0,'c',0,'k',0,'e',0,'r',0
};
static const uint8_t string3_desc[] = { 10, 3, '0',0,'0',0,'0',0,'1',0 };

/* ---- PMA helpers ---- */
static void pma_write(uint16_t off, const uint8_t *buf, uint16_t len) {
    volatile uint32_t *dst = (volatile uint32_t *)(PMA_BASE + off);
    uint16_t i;
    for (i = 0; i + 3 < len; i += 4)
        *dst++ = (uint32_t)buf[i] | ((uint32_t)buf[i+1]<<8) |
                 ((uint32_t)buf[i+2]<<16) | ((uint32_t)buf[i+3]<<24);
    if (i < len) {
        uint32_t v = 0;
        for (uint16_t j = 0; j < (len - i); j++) v |= ((uint32_t)buf[i+j]) << (j*8);
        *dst = v;
    }
}

static void pma_read(uint16_t off, uint8_t *buf, uint16_t len) {
    volatile uint32_t *src = (volatile uint32_t *)(PMA_BASE + off);
    uint16_t i;
    for (i = 0; i + 3 < len; i += 4) {
        uint32_t v = *src++;
        buf[i] = v; buf[i+1] = v>>8; buf[i+2] = v>>16; buf[i+3] = v>>24;
    }
    if (i < len) {
        uint32_t v = *src;
        for (uint16_t j = 0; j < (len-i); j++) buf[i+j] = (v >> (j*8)) & 0xFF;
    }
}

/* ---- BDT helpers ---- */
static void bdt_set_tx(uint8_t ep, uint16_t addr, uint16_t count) {
    BDT_TXBD(ep) = ((uint32_t)count << 16) | (addr & ~3U);
}
static void bdt_set_tx_count(uint8_t ep, uint16_t count) {
    uint32_t v = BDT_TXBD(ep);
    BDT_TXBD(ep) = (v & 0x0000FFFFUL) | ((uint32_t)count << 16);
}
static void bdt_set_rx(uint8_t ep, uint16_t addr, uint32_t cfg) {
    BDT_RXBD(ep) = cfg | (addr & ~3U);
}
static uint16_t bdt_get_rx_count(uint8_t ep) {
    return (BDT_RXBD(ep) >> 16) & 0x3FF;
}
static void bdt_restore_rx_config(uint8_t ep, uint32_t cfg) {
    uint32_t v = BDT_RXBD(ep);
    BDT_RXBD(ep) = (v & 0x0000FFFFU) | cfg;
}

/* ---- EP register helpers ---- */
static void ep_set_tx_status(uint8_t ep, uint32_t s) {
    uint32_t v = USB_CHEP(ep);
    USB_CHEP(ep) = (v & EP_RW_BITS) | USB_EP_CTR_RX | USB_EP_CTR_TX | ((v ^ s) & USB_EP_STAT_TX);
}
static void ep_set_rx_status(uint8_t ep, uint32_t s) {
    uint32_t v = USB_CHEP(ep);
    USB_CHEP(ep) = (v & EP_RW_BITS) | USB_EP_CTR_RX | USB_EP_CTR_TX | ((v ^ s) & USB_EP_STAT_RX);
}
static void ep_clear_ctr_rx(uint8_t ep) {
    uint32_t v = USB_CHEP(ep);
    USB_CHEP(ep) = (v & EP_RW_BITS) | USB_EP_CTR_TX;
}
static void ep_clear_ctr_tx(uint8_t ep) {
    uint32_t v = USB_CHEP(ep);
    USB_CHEP(ep) = (v & EP_RW_BITS) | USB_EP_CTR_RX;
}

static void ep0_arm_rx(void) {
    bdt_restore_rx_config(0, PMA_RXBD_64);
    ep_set_rx_status(0, USB_EP_RX_VALID);
}
static void ep2_arm_rx(void) {
    bdt_restore_rx_config(2, PMA_RXBD_64);
    ep_set_rx_status(2, USB_EP_RX_VALID);
}

static void ep0_tx(const uint8_t *d, uint16_t len) {
    if (len > 64) len = 64;
    pma_write(EP0_TX_BUF, d, len);
    bdt_set_tx_count(0, len);
    ep_set_tx_status(0, USB_EP_TX_VALID);
}
static void ep0_tx_zlp(void) {
    bdt_set_tx_count(0, 0);
    ep_set_tx_status(0, USB_EP_TX_VALID);
}
static void ep1_tx(const uint8_t *d, uint16_t len) {
    if (len > 64) len = 64;
    pma_write(EP1_TX_BUF, d, len);
    bdt_set_tx_count(1, len);
    ep1_tx_busy = 1;
    ep_set_tx_status(1, USB_EP_TX_VALID);
}

/* ---- USB Reset handler ---- */
static void usb_reset(void) {
    bdt_set_tx(0, EP0_TX_BUF, 0);
    bdt_set_rx(0, EP0_RX_BUF, PMA_RXBD_64);
    bdt_set_tx(1, EP1_TX_BUF, 0);
    bdt_set_rx(1, 0, 0);
    bdt_set_tx(2, 0, 0);
    bdt_set_rx(2, EP2_RX_BUF, PMA_RXBD_64);
    bdt_set_tx(3, EP3_TX_BUF, 0);
    bdt_set_rx(3, 0, 0);

    /* Reset toggle bits for all 4 EPs */
    for (int i = 0; i < 4; i++) {
        uint32_t v = USB_CHEP(i);
        uint32_t toggles = v & (USB_EP_STAT_TX | USB_EP_STAT_RX | USB_EP_DTOG_TX | USB_EP_DTOG_RX);
        if (toggles)
            USB_CHEP(i) = (v & EP_RW_BITS) | USB_EP_CTR_RX | USB_EP_CTR_TX | toggles;
    }

    USB_CHEP(0) = USB_EP_CONTROL | USB_EP_RX_VALID | USB_EP_TX_NAK | 0;
    USB_CHEP(1) = USB_EP_BULK | USB_EP_TX_NAK | 1;
    USB_CHEP(2) = USB_EP_BULK | USB_EP_RX_VALID | 2;
    USB_CHEP(3) = USB_EP_INTERRUPT | USB_EP_TX_NAK | 3;

    USB_DADDR = 0x80;
    usb_address = 0;
    usb_set_address_pending = 0;
    usb_configured = 0;
    ep1_tx_busy = 0;
}

/* ---- Setup handler ---- */
static uint8_t setup_buf[8];
static const uint8_t *tx_ptr;
static uint16_t tx_remaining;

static void handle_setup(void) {
    pma_read(EP0_RX_BUF, setup_buf, 8);

    uint8_t bmReq = setup_buf[0];
    uint8_t bReq  = setup_buf[1];
    uint16_t wVal = setup_buf[2] | (setup_buf[3] << 8);
    uint16_t wLen = setup_buf[6] | (setup_buf[7] << 8);

    tx_ptr = 0; tx_remaining = 0;

    if ((bmReq & 0x60) == 0x00) {
        switch (bReq) {
        case 0x06: { /* GET_DESCRIPTOR */
            uint8_t dt = wVal >> 8, di = wVal & 0xFF;
            const uint8_t *d = 0; uint16_t dl = 0;
            switch (dt) {
            case 1: d = device_desc; dl = sizeof(device_desc); break;
            case 2: d = config_desc; dl = sizeof(config_desc); break;
            case 3:
                switch (di) {
                case 0: d = string0_desc; dl = string0_desc[0]; break;
                case 1: d = string1_desc; dl = string1_desc[0]; break;
                case 2: d = string2_desc; dl = string2_desc[0]; break;
                case 3: d = string3_desc; dl = string3_desc[0]; break;
                } break;
            case 6: ep_set_tx_status(0, USB_EP_TX_STALL); ep0_arm_rx(); return;
            }
            if (d) {
                if (wLen > 0 && dl > wLen) dl = wLen;
                uint16_t c = dl > 64 ? 64 : dl;
                ep0_tx(d, c); tx_ptr = d + c; tx_remaining = dl - c;
                ep0_arm_rx(); return;
            }
            ep_set_tx_status(0, USB_EP_TX_STALL); ep0_arm_rx(); return;
        }
        case 0x05: usb_address = wVal & 0x7F; usb_set_address_pending = 1;
                   ep0_tx_zlp(); ep0_arm_rx(); return;
        case 0x09: usb_configured = (wVal != 0); ep0_tx_zlp(); ep0_arm_rx(); return;
        case 0x00: { static const uint8_t st[2]={0,0}; ep0_tx(st,2); ep0_arm_rx(); return; }
        case 0x01: case 0x03: ep0_tx_zlp(); ep0_arm_rx(); return;
        }
    } else if ((bmReq & 0x60) == 0x20) {
        switch (bReq) {
        case 0x20: ep0_arm_rx(); return;
        case 0x21: ep0_tx(line_coding, 7); ep0_arm_rx(); return;
        case 0x22: ep0_tx_zlp(); ep0_arm_rx(); return;
        }
    }
    ep_set_tx_status(0, USB_EP_TX_STALL);
    ep_set_rx_status(0, USB_EP_RX_STALL);
}

/* ---- Public API ---- */

void usb_cdc_set_rx_callback(cdc_rx_callback_t cb) { rx_callback = cb; }
bool usb_cdc_is_configured(void) { return usb_configured; }

void usb_cdc_init(void) {
    /* GPIO PA11/PA12 → AF10 (USB) */
    RCC_IOPENR |= (1 << 0);
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    uint32_t m = GPIO_MODER(GPIOA_BASE);
    m &= ~((3<<22)|(3<<24)); m |= ((2<<22)|(2<<24));
    GPIO_MODER(GPIOA_BASE) = m;
    uint32_t a = GPIO_AFRH(GPIOA_BASE);
    a &= ~((0xF<<12)|(0xF<<16)); a |= ((10<<12)|(10<<16));
    GPIO_AFRH(GPIOA_BASE) = a;
    GPIO_OSPEEDR(GPIOA_BASE) |= (3<<22)|(3<<24);

    /* Enable USB clock, CRS, PWR */
    RCC_APBENR1 |= (1<<28)|(1<<16)|(1<<13);
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    /* HSI48 */
    RCC_CRRCR |= (1<<0);
    while (!(RCC_CRRCR & (1<<1)));
    RCC_CCIPR = (RCC_CCIPR & ~(3<<26)) | (3<<26);

    /* CRS sync from USB SOF */
    CRS_CFGR = (CRS_CFGR & ~(3<<28)) | (2<<28);
    CRS_CR |= (1<<5)|(1<<6);

    PWR_CR2 |= (1<<10);  /* USV */

    /* USB init sequence */
    USB_CNTR = (1<<0);  /* USBRST=1, PDWN=0 */
    for (volatile uint32_t i = 0; i < 20000; i++) __asm__("nop");
    USB_CNTR = 0;
    for (volatile uint32_t i = 0; i < 20000; i++) __asm__("nop");

    for (int i = 0; i < 1024; i += 4)
        *(volatile uint32_t *)(PMA_BASE + i) = 0;

    USB_ISTR = 0;
    USB_BCDR |= USB_BCDR_DPPU;
    USB_CNTR = USB_CNTR_CTRM | USB_CNTR_RESETM | USB_CNTR_SUSPM |
               USB_CNTR_WKUPM | USB_CNTR_ERRM | USB_CNTR_ESOFM | USB_CNTR_SOFM;
    USB_DADDR = 0x80;
}

void usb_cdc_poll(void) {
    uint32_t istr = USB_ISTR;

    if (istr & USB_ISTR_RESET) {
        USB_ISTR = ~USB_ISTR_RESET;
        usb_reset();
        return;
    }

    if (istr & USB_ISTR_CTR) {
        uint8_t ep_num = istr & USB_ISTR_EP_ID;
        uint32_t ep_val = USB_CHEP(ep_num);

        if (ep_num == 0) {
            if (ep_val & USB_EP_CTR_RX) {
                if (ep_val & USB_EP_SETUP) {
                    for (volatile int i = 0; i < 20; i++) __asm__("nop");
                    ep_clear_ctr_rx(0);
                    handle_setup();
                } else {
                    ep_clear_ctr_rx(0);
                    uint16_t cnt = bdt_get_rx_count(0);
                    if (cnt == 7) pma_read(EP0_RX_BUF, line_coding, 7);
                    ep0_tx_zlp();
                    ep0_arm_rx();
                }
            }
            if (ep_val & USB_EP_CTR_TX) {
                ep_clear_ctr_tx(0);
                if (usb_set_address_pending) {
                    USB_DADDR = 0x80 | usb_address;
                    usb_set_address_pending = 0;
                }
                if (tx_remaining > 0 && tx_ptr) {
                    uint16_t c = tx_remaining > 64 ? 64 : tx_remaining;
                    ep0_tx(tx_ptr, c);
                    tx_ptr += c; tx_remaining -= c;
                }
                ep0_arm_rx();
            }
        } else if (ep_num == 1) {
            if (ep_val & USB_EP_CTR_TX) { ep_clear_ctr_tx(1); ep1_tx_busy = 0; }
        } else if (ep_num == 2) {
            if (ep_val & USB_EP_CTR_RX) {
                ep_clear_ctr_rx(2);
                uint16_t cnt = bdt_get_rx_count(2);
                if (cnt > 0 && cnt <= 64) {
                    uint8_t rxbuf[64];
                    pma_read(EP2_RX_BUF, rxbuf, cnt);
                    if (rx_callback)
                        rx_callback(rxbuf, cnt);
                }
                ep2_arm_rx();
            }
        } else if (ep_num == 3) {
            if (ep_val & USB_EP_CTR_TX) ep_clear_ctr_tx(3);
        }
    }

    if (istr & USB_ISTR_SUSP) { USB_ISTR = ~USB_ISTR_SUSP; USB_CNTR |= USB_CNTR_SUSPM; }
    if (istr & USB_ISTR_WKUP) USB_ISTR = ~USB_ISTR_WKUP;
    if (istr & USB_ISTR_SOF)  USB_ISTR = ~USB_ISTR_SOF;
    if (istr & USB_ISTR_ERR)  USB_ISTR = ~USB_ISTR_ERR;
    if (istr & USB_ISTR_PMAOVR) USB_ISTR = ~USB_ISTR_PMAOVR;
    if (istr & USB_ISTR_ESOF) USB_ISTR = ~USB_ISTR_ESOF;
}

bool cdc_print_str(const char *str) {
    if (!usb_configured) return false;
    uint16_t total = 0;
    while (str[total]) total++;
    if (!total) return false;

    const uint8_t *p = (const uint8_t *)str;
    uint16_t remaining = total;

    while (remaining > 0) {
        /* Wait for EP1 free */
        for (volatile int t = 0; t < 100000 && ep1_tx_busy; t++)
            usb_cdc_poll();

        if (ep1_tx_busy) return false;

        uint16_t chunk = remaining > 64 ? 64 : remaining;
        ep1_tx(p, chunk);
        p += chunk;
        remaining -= chunk;
    }
    return true;
}
