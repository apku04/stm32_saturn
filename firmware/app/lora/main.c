/*
 * main.c — STM32U073 LoRa firmware
 * Ported from PIC24 pic24LoraAlpha project
 *
 * Architecture: polled main loop (no ISRs), same as PIC24 version
 * Layers: Radio → MAC → Network → App → Terminal (USB CDC)
 */

#include <stdint.h>
#include <string.h>
#include "stm32u0.h"
#include "hw_pins.h"
#include "globalInclude.h"
#include "usb_cdc.h"
#include "spi.h"
#include "timer.h"
#include "radio.h"
#include "maclayer.h"
#include "networklayer.h"
#include "packetBuffer.h"
#include "terminal.h"
#include "hal.h"
#include "adc.h"
#include "ina219.h"

/* ---- Globals ---- */
static uint16_t seq = 0;
static PacketBuffer pRxBuf, pTxBuf;

extern uint8_t get_beacon_flag(void);

/* ---- Forward declarations ---- */
static void clock_init(void);
static void led_init(void);
static void gpio_init(void);
static void beaconHandler(void);
static void dcf_app_interface(Direction dir, PacketBuffer *rx, PacketBuffer *tx);
static void app_outgoing(Packet *pkt, PacketBuffer *txbuf);
static void app_incoming(Packet *pkt, PacketBuffer *txbuf);
static void usb_rx_handler(uint8_t *data, uint16_t len);

/* ---- LED helpers (PB13 = LED1, PB14 = LED2, active high) ---- */
void led1_toggle(void) { GPIO_ODR(GPIOB_BASE) ^= (1 << LED1_PIN); }
static void led2_toggle(void) { GPIO_ODR(GPIOB_BASE) ^= (1 << LED2_PIN); }
static void led1_on(void) { GPIO_BSRR(GPIOB_BASE) = (1 << LED1_PIN); }

/* ---- Clock: HSI16 as SYSCLK, HSI48 for USB ---- */
static void clock_init(void) {
    RCC_APBENR1 |= (1 << 28);  /* PWREN */
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    /* HSI16 on, switch sysclk */
    RCC_CR |= (1 << 8);
    while (!(RCC_CR & (1 << 10)));
    RCC_CFGR = (RCC_CFGR & ~7U) | 1U;
    while ((RCC_CFGR & (7U << 3)) != (1U << 3));
}

/* ---- LED GPIO init (PB13 = LED1, PB14 = LED2, push-pull output) ---- */
static void led_init(void) {
    RCC_IOPENR |= (1 << 1);  /* GPIOB clock */
    uint32_t m = GPIO_MODER(GPIOB_BASE);
    m &= ~((3 << (LED1_PIN * 2)) | (3 << (LED2_PIN * 2)));
    m |=  ((1 << (LED1_PIN * 2)) | (1 << (LED2_PIN * 2)));  /* output */
    GPIO_MODER(GPIOB_BASE) = m;
}

/* ---- General GPIO for radio, etc. ---- */
static void gpio_init(void) {
    /* Enable GPIOA and GPIOB clocks */
    RCC_IOPENR |= (1 << 0) | (1 << 1);
    for (volatile int i = 0; i < 10; i++) __asm__("nop");
}

/* ---- USB receive callback → terminal ---- */
static void usb_rx_handler(uint8_t *data, uint16_t len) {
    if (len > 0 && len < 255)
        terminal(data, (uint8_t)len);
}

/* ---- Beacon handler (called every 10s by timer) ---- */
static void beaconHandler(void) {
    if (GLOB_ERROR_BUFFER_FULL != buffer_full(&pTxBuf)) {
        if (get_beacon_flag()) {
            Packet pkt;
            memset(&pkt, 0, sizeof(Packet));
            seq++;
            pkt.pOwner = NET;
            pkt.pktDir = OUTGOING;
            pkt.control_app = BEACON;
            pkt.sequence_num = seq;

            /* Beacon payload: i_ma(2, signed) + bus_mv(2) + bat_mv(2) + charge_status(1)
             * Current is computed on the source so the receiver doesn't need to know
             * the shunt resistor value. R58 = 50 mΩ → I[mA] = V_shunt[µV] / 50. */
            int32_t  sh_uv    = ina219_read_shunt_uv();
            int16_t  i_ma     = (int16_t)(sh_uv / 50);
            uint16_t bus_mv   = ina219_read_bus_mv();
            /* ADC bat sense not usable (PB4 has no ADC on STM32U073) —
               using INA219 bus voltage as battery proxy */
            uint16_t bat_mv   = bus_mv;
            uint8_t  chg      = (uint8_t)charge_get_status();
            pkt.data[0] = (uint8_t)(i_ma & 0xFF);
            pkt.data[1] = (uint8_t)((uint16_t)i_ma >> 8);
            pkt.data[2] = (uint8_t)(bus_mv & 0xFF);
            pkt.data[3] = (uint8_t)(bus_mv >> 8);
            pkt.data[4] = (uint8_t)(bat_mv & 0xFF);
            pkt.data[5] = (uint8_t)(bat_mv >> 8);
            pkt.data[6] = chg;
            /* v4: append remote radio config so base can see what tx_pwr/sf
             * the field device is currently using. */
            pkt.data[7] = radio_get_tx_power();
            pkt.data[8] = radio_get_datarate();
            pkt.length = 4 + 9;  /* header + 9 bytes telemetry (v4) */

            write_packet(&pTxBuf, &pkt);
        }
    }
}

/* ---- App layer interface ---- */
static void dcf_app_interface(Direction dir, PacketBuffer *rx, PacketBuffer *tx) {
    if (dir == INCOMING) {
        Packet pkt = search_pending_packet(rx, INCOMING, APP);
        if (pkt.pktDir != DIR_EMPTY && pkt.pOwner != OWNER_EMPTY)
            app_incoming(&pkt, tx);
    } else if (dir == OUTGOING) {
        Packet pkt = search_pending_packet(tx, OUTGOING, APP);
        if (pkt.pktDir != DIR_EMPTY && pkt.pOwner != OWNER_EMPTY)
            app_outgoing(&pkt, tx);
    }
}

static void app_outgoing(Packet *pkt, PacketBuffer *txbuf) {
    if (GLOB_ERROR_BUFFER_FULL != buffer_full(txbuf)) {
        seq++;
        pkt->pOwner = NET;
        pkt->pktDir = OUTGOING;
        pkt->control_app = PAYLOAD;
        pkt->length = pkt->length + 4;
        pkt->sequence_num = seq;
        write_packet(txbuf, pkt);
    }
}

static void app_incoming(Packet *pkt, PacketBuffer *txbuf) {
    /* Print received packet info */
    char buf[160];
    snprintf(buf, sizeof(buf),
             "[RX] src=%u dst=%u rssi=%d prssi=%d snr=%d sf=%u freq=%lu type=%u seq=%u len=%u\n",
             pkt->source_adr, pkt->destination_adr, pkt->rssi, pkt->prssi,
             pkt->snr, radio_get_datarate(), (unsigned long)radio_get_channel(),
             pkt->control_app, pkt->sequence_num, pkt->length);
    print(buf);

    /* Decode beacon telemetry */
    if (pkt->control_app == BEACON) {
        uint8_t data_len = 0;
        if (pkt->length > PACKET_HEADER_SIZE)
            data_len = pkt->length - PACKET_HEADER_SIZE;
        uint8_t tbl_bytes = pkt->mesh_tbl_entries * 3;
        uint8_t app_off = tbl_bytes;
        if (data_len >= app_off + 9) {
            /* v4: i_ma(2) + bus(2) + bat(2) + chg(1) + tx_pwr(1) + sf(1) */
            int16_t  i_ma  = (int16_t)(pkt->data[app_off] | ((uint16_t)pkt->data[app_off+1] << 8));
            uint16_t bus   = pkt->data[app_off+2] | ((uint16_t)pkt->data[app_off+3] << 8);
            uint16_t bat   = pkt->data[app_off+4] | ((uint16_t)pkt->data[app_off+5] << 8);
            uint8_t  chg   = pkt->data[app_off+6];
            uint8_t  txp   = pkt->data[app_off+7];
            uint8_t  sf    = pkt->data[app_off+8];
            snprintf(buf, sizeof(buf),
                     "[BEACON] i_ma=%d bus=%u bat=%u chg=%u tx_pwr=%u sf=%u entries=%u\n",
                     i_ma, bus, bat, chg, txp, sf, pkt->mesh_tbl_entries);
            print(buf);
        } else if (data_len >= app_off + 7) {
            /* New format: i_ma(2,signed) + bus(2) + bat(2) + chg(1) */
            int16_t  i_ma  = (int16_t)(pkt->data[app_off] | ((uint16_t)pkt->data[app_off+1] << 8));
            uint16_t bus   = pkt->data[app_off+2] | ((uint16_t)pkt->data[app_off+3] << 8);
            uint16_t bat   = pkt->data[app_off+4] | ((uint16_t)pkt->data[app_off+5] << 8);
            uint8_t  chg   = pkt->data[app_off+6];
            snprintf(buf, sizeof(buf), "[BEACON] i_ma=%d bus=%u bat=%u chg=%u entries=%u\n",
                     i_ma, bus, bat, chg, pkt->mesh_tbl_entries);
            print(buf);
        } else if (data_len >= app_off + 5) {
            /* Legacy: bat + sol + chg */
            uint16_t bat = pkt->data[app_off] | ((uint16_t)pkt->data[app_off+1] << 8);
            uint16_t sol = pkt->data[app_off+2] | ((uint16_t)pkt->data[app_off+3] << 8);
            uint8_t  chg = pkt->data[app_off+4];
            snprintf(buf, sizeof(buf), "[BEACON] bat=%u sol=%u chg=%u entries=%u\n",
                     bat, sol, chg, pkt->mesh_tbl_entries);
            print(buf);
        } else if (data_len >= app_off + 2) {
            /* Legacy: battery only */
            uint16_t bat = pkt->data[app_off] | ((uint16_t)pkt->data[app_off+1] << 8);
            snprintf(buf, sizeof(buf), "[BEACON] bat=%u entries=%u\n",
                     bat, pkt->mesh_tbl_entries);
            print(buf);
        } else {
            /* No telemetry (e.g. PIC24 node) */
            snprintf(buf, sizeof(buf), "[BEACON] entries=%u\n",
                     pkt->mesh_tbl_entries);
            print(buf);
        }
    } else {
        /* Print data payload as hex for non-beacon packets */
        uint8_t data_len = 0;
        if (pkt->length > PACKET_HEADER_SIZE)
            data_len = pkt->length - PACKET_HEADER_SIZE;
        if (data_len > 0 && data_len < sizeof(pkt->data)) {
            char hbuf[128];
            uint8_t n = data_len > 40 ? 40 : data_len;
            int pos = 0;
            for (uint8_t i = 0; i < n && pos < (int)sizeof(hbuf) - 4; i++)
                pos += snprintf(hbuf + pos, sizeof(hbuf) - pos, "%02X ", pkt->data[i]);
            hbuf[pos++] = '\n';
            hbuf[pos] = '\0';
            print(hbuf);
        }
    }

    /* Reply to PING */
    if (pkt->control_app == PING) {
        Packet resp;
        memset(&resp, 0, sizeof(Packet));
        resp.pOwner = NET;
        resp.pktDir = OUTGOING;
        resp.control_app = PONG;
        resp.length = 4;
        if (buffer_full(txbuf) != GLOB_ERROR_BUFFER_FULL)
            write_packet(txbuf, &resp);
    }
}

/* ---- HardFault handler ---- */
void HardFault_Handler(void) {
    RCC_IOPENR |= (1 << 1);
    for (volatile int i = 0; i < 10; i++) __asm__("nop");
    uint32_t m = GPIO_MODER(GPIOB_BASE);
    m &= ~((3<<26)|(3<<28)); m |= ((1<<26)|(1<<28));
    GPIO_MODER(GPIOB_BASE) = m;
    while (1) {
        GPIO_BSRR(GPIOB_BASE) = (1<<13)|(1<<14);
        for (volatile int i = 0; i < 100000; i++) __asm__("nop");
        GPIO_BSRR(GPIOB_BASE) = (1<<(13+16))|(1<<(14+16));
        for (volatile int i = 0; i < 100000; i++) __asm__("nop");
    }
}

/* ---- Reset handler (entry point) ---- */
void Reset_Handler(void) {
    /* Copy .data, zero .bss */
    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    dst = &_sbss;
    while (dst < &_ebss) *dst++ = 0;

    /* Init hardware */
    clock_init();
    led_init();
    gpio_init();
    led1_on();

    timer_init();
    spi_init();
    adc_init();
    ina219_init();
    usb_cdc_init();
    usb_cdc_set_rx_callback(usb_rx_handler);

    /* Init protocol stack */
    init_packet_buffer(&pRxBuf);
    init_packet_buffer(&pTxBuf);
    user_layer_init(&pTxBuf);

    register_timer_cb(beaconHandler);

    network_layer_init(&pRxBuf, &pTxBuf);
    mac_layer_init(&pRxBuf, &pTxBuf);

    /* Print node address so the user can identify this board */
    {
        char abuf[32];
        snprintf(abuf, sizeof(abuf), "[BOOT] node_addr=%u\n", get_mac_address());
        print(abuf);
    }

    /* Init radio — continue even if it fails (USB still works) */
    uint8_t radio_ok = 0;
    if (radio_init(&pRxBuf, &pTxBuf) == 0) {
        radio_start_rx();
        radio_ok = 1;
    }

    /* Main loop — fully polled, same structure as PIC24 */
    while (1) {
        usb_cdc_poll();
        timer_poll();

        if (radio_ok) {
            radio_irq_handler();

            dcf_app_interface(INCOMING, &pRxBuf, &pTxBuf);
            dcf_app_interface(OUTGOING, &pRxBuf, &pTxBuf);
            network_interface(INCOMING);
            network_interface(OUTGOING);
            mac_interface(INCOMING);
            mac_interface(OUTGOING);
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
