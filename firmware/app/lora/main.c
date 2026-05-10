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
#include "gps.h"
#include "bme280.h"
#include "sht3x.h"
#include "ext_flash.h"
#include "event_log.h"

/* ---- Globals ---- */
static uint16_t seq = 0;
static PacketBuffer pRxBuf, pTxBuf;

extern uint8_t get_beacon_flag(void);

/* ---- Brown-out / hang diagnostics ----
 * Stored in .noinit RAM: survives soft / IWDG / BOR resets, lost on full
 * power-cycle. Lets us answer 'why did MAC 44 wedge in the field?' from the
 * gateway log alone (the values are telemetered in every beacon). */
#define DIAG_MAGIC 0xD1A6B007u
typedef struct {
    uint32_t magic;
    uint32_t boot_count;        /* increments every reset since power-up */
    uint32_t last_reset_csr;    /* RCC_CSR snapshot from THIS reset */
    uint32_t last_init_stage;   /* highest stage reached on PREVIOUS boot */
    uint32_t init_stage;        /* live stage counter for THIS boot */
    uint32_t last_uptime_ms;    /* uptime saved each beacon — survives reset */
} diag_t;
__attribute__((section(".noinit"))) static diag_t diag;

/* ---- Forward declarations ---- */
static void clock_init(void);
static void led_init(void);
static void gpio_init(void);
static void wdt_init(void);
static void wdt_kick(void);
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

/* ---- IWDG: independent watchdog (LSI ~32 kHz) ----
 * PR=4 (/64), RLR=4000 → ~8 s timeout.
 * Recovers from any code path that hangs longer than 8s — including
 * SX1262 BUSY hangs and brownout-induced corruption that survives
 * after the rail stabilises.
 */
static void wdt_init(void) {
    IWDG_KR = IWDG_KR_START;     /* Start the watchdog (also enables LSI) */
    IWDG_KR = IWDG_KR_UNLOCK;    /* Unlock PR/RLR */
    /* Wait for any pending update to complete before writing */
    while (IWDG_SR & 0x07);
    IWDG_PR  = 4;                /* Prescaler /64 → 500 Hz */
    while (IWDG_SR & 0x01);      /* PVU */
    IWDG_RLR = 4000;             /* 4000 / 500 Hz = 8 s */
    while (IWDG_SR & 0x02);      /* RVU */
    IWDG_KR = IWDG_KR_REFRESH;   /* Initial refresh */
}

static void wdt_kick(void) {
    IWDG_KR = IWDG_KR_REFRESH;
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

            /* v5: append GPS position data (9 bytes) */
            const gps_fix_t *gfix = gps_get_fix();
            int32_t lat_udeg = gfix->valid ? gfix->lat_udeg : 0;
            int32_t lon_udeg = gfix->valid ? gfix->lon_udeg : 0;
            memcpy(&pkt.data[9],  &lat_udeg, 4);
            memcpy(&pkt.data[13], &lon_udeg, 4);
            pkt.data[17] = gfix->valid ? 1u : 0u;

            /* v6: append temperature + humidity (4 bytes).
             * Prefer SHT3x (real RH), fall back to BME280/BMP280.
             * If neither is present we still ship zeros so the decoder
             * can rely on the fixed payload length. */
            int16_t  temp_cdeg = 0;
            uint16_t hum_cpct  = 0;
            /* If SHT3x didn't come up at boot (e.g. sensor wasn't powered yet
             * or was in a confused state after DFU reflash), retry init each
             * beacon cycle so it can self-recover without a power cycle. */
            if (!sht3x_present()) sht3x_init();
            if (sht3x_present() && sht3x_sample() == 0) {
                temp_cdeg = sht3x_get_temp_cdeg();
                hum_cpct  = sht3x_get_hum_cpct();
            } else if (bme280_present() && bme280_sample() == 0) {
                temp_cdeg = bme280_get_temp_cdeg();
                hum_cpct  = bme280_get_hum_cpct();   /* 0 on BMP280 */
            }
            pkt.data[18] = (uint8_t)(temp_cdeg & 0xFF);
            pkt.data[19] = (uint8_t)((uint16_t)temp_cdeg >> 8);
            pkt.data[20] = (uint8_t)(hum_cpct  & 0xFF);
            pkt.data[21] = (uint8_t)(hum_cpct  >> 8);

            /* v7: append brown-out / hang diagnostics (4 bytes).
             * Lets the gateway see WHY a remote node restarted without
             * needing to retrieve it. Compact reset_cause = bits 23..30
             * of the CSR snapshot, packed into one byte (LSB = bit 23). */
            uint8_t rst_compact = (uint8_t)((diag.last_reset_csr >> 23) & 0xFF);
            uint8_t boot_cnt    = diag.boot_count > 255u ? 255u
                                                          : (uint8_t)diag.boot_count;
            uint8_t last_stage  = (uint8_t)diag.last_init_stage;
            uint32_t up_ms      = get_tick_ms();
            uint32_t up_min32   = up_ms / 60000u;
            uint8_t  up_min     = up_min32 > 255u ? 255u : (uint8_t)up_min32;
            pkt.data[22] = rst_compact;
            pkt.data[23] = boot_cnt;
            pkt.data[24] = last_stage;
            pkt.data[25] = up_min;
            pkt.length = 4 + 26;  /* header + 26 bytes telemetry (v7) */

            /* Persist current uptime so a future reset can report 'last
             * known good uptime' — helps diagnose intermittent dies. */
            diag.last_uptime_ms = up_ms;

            write_packet(&pTxBuf, &pkt);

            /* Self-print so the monitor can show this node's own readings
             * (the local board never receives its own TX over the air).
             * Format mirrors app_incoming so lora_monitor.py parses it. */
            {
                char b[200];
                uint8_t self = get_mac_address();
                snprintf(b, sizeof(b),
                    "[RX] src=%u dst=255 rssi=0 prssi=0 snr=0 sf=%u freq=%lu type=0 seq=%u len=%u\n",
                    self, radio_get_datarate(),
                    (unsigned long)radio_get_channel(), seq, pkt.length);
                print(b);
                snprintf(b, sizeof(b),
                    "[BEACON] i_ma=%d bus=%u bat=%u chg=%u tx_pwr=%u sf=%u"
                    " lat=%ld lon=%ld fix=%u temp_cdeg=%d hum_cpct=%u"
                    " rst=0x%02X boot=%u last_stage=%u up_min=%u entries=0\n",
                    i_ma, bus_mv, bat_mv, chg,
                    radio_get_tx_power(), radio_get_datarate(),
                    (long)lat_udeg, (long)lon_udeg, gfix->valid ? 1u : 0u,
                    temp_cdeg, hum_cpct,
                    rst_compact, boot_cnt, last_stage, up_min);
                print(b);
            }
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
    /* Print received packet info.
     * Buffer must fit the longest [BEACON] decode line (v7 with all fields
     * populated is ~170 chars including the trailing newline). 160 was too
     * small and the snprintf silently truncated "entries=0\n" off the end,
     * causing host-side regex to drop the entire beacon. */
    char buf[224];
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
        if (data_len >= app_off + 26) {
            /* v7: v6 fields + rst(1) + boot(1) + last_stage(1) + up_min(1) */
            int16_t  i_ma  = (int16_t)(pkt->data[app_off] | ((uint16_t)pkt->data[app_off+1] << 8));
            uint16_t bus   = pkt->data[app_off+2] | ((uint16_t)pkt->data[app_off+3] << 8);
            uint16_t bat   = pkt->data[app_off+4] | ((uint16_t)pkt->data[app_off+5] << 8);
            uint8_t  chg   = pkt->data[app_off+6];
            uint8_t  txp   = pkt->data[app_off+7];
            uint8_t  sf    = pkt->data[app_off+8];
            int32_t  lat_udeg, lon_udeg;
            memcpy(&lat_udeg, &pkt->data[app_off+9],  4);
            memcpy(&lon_udeg, &pkt->data[app_off+13], 4);
            uint8_t  fix   = pkt->data[app_off+17];
            int16_t  tcd   = (int16_t)(pkt->data[app_off+18] | ((uint16_t)pkt->data[app_off+19] << 8));
            uint16_t hcp   = pkt->data[app_off+20] | ((uint16_t)pkt->data[app_off+21] << 8);
            uint8_t  rst   = pkt->data[app_off+22];
            uint8_t  boot  = pkt->data[app_off+23];
            uint8_t  lstg  = pkt->data[app_off+24];
            uint8_t  upm   = pkt->data[app_off+25];
            snprintf(buf, sizeof(buf),
                     "[BEACON] i_ma=%d bus=%u bat=%u chg=%u tx_pwr=%u sf=%u"
                     " lat=%ld lon=%ld fix=%u temp_cdeg=%d hum_cpct=%u"
                     " rst=0x%02X boot=%u last_stage=%u up_min=%u entries=%u\n",
                     i_ma, bus, bat, chg, txp, sf,
                     (long)lat_udeg, (long)lon_udeg, fix,
                     tcd, hcp, rst, boot, lstg, upm,
                     pkt->mesh_tbl_entries);
            print(buf);
        } else if (data_len >= app_off + 22) {
            /* v6: v5 fields + temp_cdeg(2,signed) + hum_cpct(2) */
            int16_t  i_ma  = (int16_t)(pkt->data[app_off] | ((uint16_t)pkt->data[app_off+1] << 8));
            uint16_t bus   = pkt->data[app_off+2] | ((uint16_t)pkt->data[app_off+3] << 8);
            uint16_t bat   = pkt->data[app_off+4] | ((uint16_t)pkt->data[app_off+5] << 8);
            uint8_t  chg   = pkt->data[app_off+6];
            uint8_t  txp   = pkt->data[app_off+7];
            uint8_t  sf    = pkt->data[app_off+8];
            int32_t  lat_udeg, lon_udeg;
            memcpy(&lat_udeg, &pkt->data[app_off+9],  4);
            memcpy(&lon_udeg, &pkt->data[app_off+13], 4);
            uint8_t  fix   = pkt->data[app_off+17];
            int16_t  tcd   = (int16_t)(pkt->data[app_off+18] | ((uint16_t)pkt->data[app_off+19] << 8));
            uint16_t hcp   = pkt->data[app_off+20] | ((uint16_t)pkt->data[app_off+21] << 8);
            snprintf(buf, sizeof(buf),
                     "[BEACON] i_ma=%d bus=%u bat=%u chg=%u tx_pwr=%u sf=%u"
                     " lat=%ld lon=%ld fix=%u temp_cdeg=%d hum_cpct=%u entries=%u\n",
                     i_ma, bus, bat, chg, txp, sf,
                     (long)lat_udeg, (long)lon_udeg, fix,
                     tcd, hcp, pkt->mesh_tbl_entries);
            print(buf);
        } else if (data_len >= app_off + 18) {
            /* v5: v4 fields + lat_udeg(4) + lon_udeg(4) + fix_valid(1) */
            int16_t  i_ma  = (int16_t)(pkt->data[app_off] | ((uint16_t)pkt->data[app_off+1] << 8));
            uint16_t bus   = pkt->data[app_off+2] | ((uint16_t)pkt->data[app_off+3] << 8);
            uint16_t bat   = pkt->data[app_off+4] | ((uint16_t)pkt->data[app_off+5] << 8);
            uint8_t  chg   = pkt->data[app_off+6];
            uint8_t  txp   = pkt->data[app_off+7];
            uint8_t  sf    = pkt->data[app_off+8];
            int32_t  lat_udeg, lon_udeg;
            memcpy(&lat_udeg, &pkt->data[app_off+9],  4);
            memcpy(&lon_udeg, &pkt->data[app_off+13], 4);
            uint8_t  fix   = pkt->data[app_off+17];
            snprintf(buf, sizeof(buf),
                     "[BEACON] i_ma=%d bus=%u bat=%u chg=%u tx_pwr=%u sf=%u"
                     " lat=%ld lon=%ld fix=%u entries=%u\n",
                     i_ma, bus, bat, chg, txp, sf,
                     (long)lat_udeg, (long)lon_udeg, fix,
                     pkt->mesh_tbl_entries);
            print(buf);
        } else if (data_len >= app_off + 9) {
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

    /* Handle CMD_CFG on the field device: send CMD_ACK first, THEN apply
     * the new setting (so the ACK still goes out at the old power/SF and
     * is heard by the base). RAM-only — flash is not touched, so a reset
     * reverts to the configured defaults. */
    if (pkt->control_app == CMD_CFG) {
        uint8_t data_len = 0;
        if (pkt->length > PACKET_HEADER_SIZE)
            data_len = pkt->length - PACKET_HEADER_SIZE;
        uint8_t tbl_bytes = pkt->mesh_tbl_entries * 3;
        uint8_t off = tbl_bytes;

        uint8_t op = 0, new_pwr = radio_get_tx_power(), new_sf = radio_get_datarate();
        uint8_t status = 1;  /* 0=ok, 1=invalid */

        if (data_len >= off + 2) {
            op = pkt->data[off];
            if (op == 0x01 && data_len >= off + 2) {
                uint8_t v = pkt->data[off + 1];
                if (v >= 1 && v <= 22) { new_pwr = v; status = 0; }
            } else if (op == 0x02 && data_len >= off + 2) {
                uint8_t v = pkt->data[off + 1];
                if (v >= 5 && v <= 12) { new_sf = v; status = 0; }
            } else if (op == 0x03 && data_len >= off + 3) {
                uint8_t p = pkt->data[off + 1];
                uint8_t s = pkt->data[off + 2];
                if (p >= 1 && p <= 22 && s >= 5 && s <= 12) {
                    new_pwr = p; new_sf = s; status = 0;
                }
            }
        }

        /* Build CMD_ACK reply */
        Packet ack;
        memset(&ack, 0, sizeof(Packet));
        ack.pOwner = NET;
        ack.pktDir = OUTGOING;
        ack.control_app = CMD_ACK;
        ack.data[0] = op;
        ack.data[1] = status;
        ack.data[2] = new_pwr;
        ack.data[3] = new_sf;
        ack.length = 4 + 4;
        if (buffer_full(txbuf) != GLOB_ERROR_BUFFER_FULL)
            write_packet(txbuf, &ack);

        /* Apply AFTER queueing the ACK (RAM-only). */
        if (status == 0) {
            if (op == 0x01 || op == 0x03) radio_set_tx_power(new_pwr);
            if (op == 0x02 || op == 0x03) radio_set_datarate(new_sf);
            print("[CFG] applied\n");
        } else {
            print("[CFG] rejected (invalid value)\n");
        }
    }

    /* Print CMD_ACK on the base station so the operator sees confirmation. */
    if (pkt->control_app == CMD_ACK) {
        uint8_t data_len = 0;
        if (pkt->length > PACKET_HEADER_SIZE)
            data_len = pkt->length - PACKET_HEADER_SIZE;
        uint8_t tbl_bytes = pkt->mesh_tbl_entries * 3;
        uint8_t off = tbl_bytes;
        if (data_len >= off + 4) {
            char cbuf[96];
            snprintf(cbuf, sizeof(cbuf),
                     "[CMD_ACK] from=%u op=%u status=%u tx_pwr=%u sf=%u\n",
                     pkt->source_adr,
                     pkt->data[off], pkt->data[off+1],
                     pkt->data[off+2], pkt->data[off+3]);
            print(cbuf);
        }
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
    /* Set VTOR to our vector table — critical after DFU bootloader return */
    SCB_VTOR = 0x08000000u;

    /* Snapshot reset cause BEFORE anything else can touch RCC_CSR. */
    uint32_t csr_snapshot = RCC_CSR;
    RCC_CSR |= RCC_CSR_RMVF;   /* clear flags so next reset reads cleanly */

    /* Copy .data, zero .bss (does NOT touch .noinit — see linker script) */
    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    dst = &_sbss;
    while (dst < &_ebss) *dst++ = 0;

    /* Update .noinit diagnostic state */
    if (diag.magic == DIAG_MAGIC) {
        diag.boot_count++;
        diag.last_init_stage = diag.init_stage;   /* how far we got last time */
    } else {
        diag.magic = DIAG_MAGIC;
        diag.boot_count = 1;
        diag.last_init_stage = 0;
        diag.last_uptime_ms = 0;
    }
    diag.last_reset_csr = csr_snapshot;
    diag.init_stage = 1;   /* entered Reset_Handler */

    /* Init hardware */
    clock_init();
    led_init();
    gpio_init();
    wdt_init();      /* Start IWDG immediately so any later hang is recoverable */
    diag.init_stage = 2;
    led1_on();

    timer_init();      diag.init_stage = 3;
    spi_init();        diag.init_stage = 4;
    adc_init();        diag.init_stage = 5;
    ina219_init();     diag.init_stage = 6;
    wdt_kick();
    bme280_init();     diag.init_stage = 7;   /* external I2C header — PB6/PB7 bit-bang */
    sht3x_init();      diag.init_stage = 8;   /* same bus; bb_i2c_init() is idempotent  */
    ext_flash_init();  diag.init_stage = 9;
    /* Persistent boot log: append a record describing THIS reset (and what
     * happened on the previous boot). Done immediately after ext_flash so a
     * later init failure on the same boot is also captured by the NEXT boot's
     * record. We log this_init_stage=9 because that's where we are right now;
     * the host can see we got at least this far. reached_main is taken from
     * the previous boot's high-water-mark (init_stage==0xFF means main-loop). */
    event_log_init();
    event_log_append(diag.boot_count,
                     diag.last_reset_csr,
                     diag.last_uptime_ms,
                     (uint8_t)diag.last_init_stage,
                     (uint8_t)diag.init_stage,
                     diag.last_init_stage == 0xFFu);
    wdt_kick();
    gps_init();        diag.init_stage = 10;
    usb_cdc_init();    diag.init_stage = 11;
    usb_cdc_set_rx_callback(usb_rx_handler);
    wdt_kick();

    /* Init protocol stack */
    init_packet_buffer(&pRxBuf);
    init_packet_buffer(&pTxBuf);
    user_layer_init(&pTxBuf);

    register_timer_cb(beaconHandler);

    network_layer_init(&pRxBuf, &pTxBuf);
    mac_layer_init(&pRxBuf, &pTxBuf);

    /* Print node address + reset diagnostics so the user can identify this board
     * AND see why it (re)started — visible on local CDC and helpful when bench
     * debugging. The same fields are telemetered in beacons (see beaconHandler). */
    {
        char abuf[96];
        snprintf(abuf, sizeof(abuf),
                 "[BOOT] node_addr=%u rst=0x%08lX boot=%lu last_stage=%lu last_uptime_ms=%lu\n",
                 get_mac_address(),
                 (unsigned long)diag.last_reset_csr,
                 (unsigned long)diag.boot_count,
                 (unsigned long)diag.last_init_stage,
                 (unsigned long)diag.last_uptime_ms);
        print(abuf);
    }

    /* Init radio — continue even if it fails (USB still works) */
    uint8_t radio_ok = 0;
    if (radio_init(&pRxBuf, &pTxBuf) == 0) {
        radio_start_rx();
        radio_ok = 1;
    }
    diag.init_stage = 12;
    wdt_kick();

    /* Main loop — fully polled, same structure as PIC24 */
    diag.init_stage = 0xFF;   /* reached main loop successfully */
    while (1) {
        wdt_kick();
        usb_cdc_poll();
        timer_poll();
        gps_poll();

        if (radio_ok) {
            radio_irq_handler();

            dcf_app_interface(INCOMING, &pRxBuf, &pTxBuf);
            dcf_app_interface(OUTGOING, &pRxBuf, &pTxBuf);
            network_interface(INCOMING);
            network_interface(OUTGOING);
            mac_interface(INCOMING);
            mac_interface(OUTGOING);
            mac_interface(RETX);
        }
    }
}

/* ---- Vector table ---- */
extern uint32_t _estack;

/* USART2 ISR defined in gps.c */
void USART2_IRQHandler(void);

__attribute__((section(".isr_vector")))
const uint32_t vectors[] = {
    (uint32_t)&_estack,
    (uint32_t)Reset_Handler,
    (uint32_t)HardFault_Handler,  /* NMI */
    (uint32_t)HardFault_Handler,  /* HardFault */
    0, 0, 0, 0, 0, 0, 0,         /* Reserved (4-10) */
    0,                            /* SVCall (11) */
    0, 0,                         /* Reserved (12-13) */
    0,                            /* PendSV (14) */
    0,                            /* SysTick (15) */
    /* IRQ 0-27 */
    0, 0, 0, 0, 0, 0, 0, 0,      /* IRQ  0- 7 */
    0, 0, 0, 0, 0, 0, 0, 0,      /* IRQ  8-15 */
    0, 0, 0, 0, 0, 0, 0, 0,      /* IRQ 16-23 */
    0, 0, 0, 0,                   /* IRQ 24-27 */
    (uint32_t)USART2_IRQHandler,  /* IRQ 28 — USART2 */
};
