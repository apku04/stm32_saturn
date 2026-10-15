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
#include "flash_config.h"
#include "ota_apply.h"
#include "flash_ota.h"
#include "crc32.h"
#include "version.h"

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
    uint32_t main_chk;          /* live main-loop checkpoint: which call is in
                                 * progress. Survives IWDG reset so the next
                                 * boot can report exactly where the loop hung. */
} diag_t;
__attribute__((section(".noinit"))) static diag_t diag;

/* Main-loop checkpoint codes stored in diag.main_chk. On an IWDG reset the
 * value left here is the call that was executing when the watchdog bit. */
enum {
    MAINCHK_TOP       = 1,   /* top of loop, before usb poll */
    MAINCHK_USB       = 2,   /* usb_cdc_poll */
    MAINCHK_TIMER     = 3,   /* timer_poll (runs beaconHandler) */
    MAINCHK_GPS       = 4,   /* gps_poll */
    MAINCHK_RADIO_IRQ = 5,   /* radio_irq_handler (SX1262 BUSY wait lives here) */
    MAINCHK_APP_IN    = 6,   /* dcf_app_interface INCOMING */
    MAINCHK_APP_OUT   = 7,   /* dcf_app_interface OUTGOING */
    MAINCHK_NET_IN    = 8,   /* network_interface INCOMING */
    MAINCHK_NET_OUT   = 9,   /* network_interface OUTGOING */
    MAINCHK_MAC_IN    = 10,  /* mac_interface INCOMING */
    MAINCHK_MAC_OUT   = 11,  /* mac_interface OUTGOING */
    MAINCHK_MAC_RETX  = 12,  /* mac_interface RETX */
};


/* ---- OTA state (declared here so beaconHandler and main loop can see it) ---- */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_READY,      /* PREPARE accepted; erased; waiting for first DATA  */
    OTA_STATE_RECEIVING,  /* DATA packets arriving                              */
} ota_state_t;
static ota_state_t ota_state  = OTA_STATE_IDLE;
static uint32_t    ota_idle_dl = 0;

/* ---- OTA-configuration state (see CFG_OTA_* below + boot-time verify in
 * Reset_Handler). Declared here for the same reason as ota_state: beaconHandler
 * and the firmware-OTA busy checks need to see it too, so the two "own the
 * radio + flash" protocols never run concurrently. ---- */
typedef enum {
    CFGOTA_IDLE = 0,
    CFGOTA_READY,      /* PREPARE accepted; waiting for DATA   */
    CFGOTA_RECEIVED,   /* DATA accepted; waiting for END/commit */
} cfgota_state_t;
static cfgota_state_t cfgota_state        = CFGOTA_IDLE;
static uint32_t        cfgota_idle_dl     = 0;
/* true from boot (if a provisional config was pending) until the post-reboot
 * link-verify ping either succeeds (commit) or exhausts retries (rollback). */
static bool             cfgota_verify_active = false;

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
static void persist_beacon_interval_s(uint8_t seconds);

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
    /* The USB-CDC OUT endpoint is 64 bytes, so a command longer than one packet
     * (e.g. `send ota data <t> <off> <80 hex>` ≈ 100 chars) arrives in several
     * callbacks. Accumulate bytes here until a line terminator, then dispatch
     * the whole line — otherwise long commands are split and mis-parsed. */
    static uint8_t line[480];   /* matches terminal()'s MAX_ARGV input limit
                                 * (holds an OTA DATA relay with a 192B chunk) */
    static uint16_t pos = 0;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                terminal(line, pos);
                pos = 0;
            }
        } else if (pos < sizeof(line) - 1) {
            line[pos++] = c;
        } else {
            pos = 0;   /* oversized line — drop it */
        }
    }
}

static void persist_beacon_interval_s(uint8_t seconds) {
    writeFlashParamByte(PARAMETER_N, seconds);
}

/* ---- Beacon handler (called every 10s by timer) ---- */
static void beaconHandler(void) {
    /* Suspend beacons during firmware OTA, config OTA transfer, and the
     * post-reboot config-OTA link verification — all three want a quiet
     * radio and an undistracted main loop. */
    if (ota_state != OTA_STATE_IDLE || cfgota_state != CFGOTA_IDLE ||
        cfgota_verify_active) return;
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

/* ---- OTA-over-LoRa receive (this node as target) + ACK observation (gw) ----
 *
 * OTA control packets ride the same single-hop broadcast path as CMD_CFG. Each
 * carries a target node id so only the intended node acts; everyone else drops
 * it. Wire format (payload at data[off], off = mesh_tbl_entries*3 = 0 for these
 * un-shifted control packets):
 *   OTA_BEGIN: target(1) size_lo size_hi
 *   OTA_DATA : target(1) off_lo off_hi len(1) bytes[len]   (off,len multiples of 8)
 *   OTA_END  : target(1) size_lo size_hi crc32[4]          (crc32 = zlib over image)
 *   OTA_ACK  : op(1) status(1) off_lo off_hi               (target -> sender)
 */
#define OTA_OP_PREPARE 0   /* handshake: version check + erase + enter RX mode */
#define OTA_OP_BEGIN   1   /* kept for backward compat                         */
#define OTA_OP_DATA    2
#define OTA_OP_END     3

/* PREPARE NACK reason codes (returned in ACK status byte) */
#define OTA_PREPARE_OK           0
#define OTA_PREPARE_SAME_VER     1   /* already running this build */
#define OTA_PREPARE_TOO_LARGE    2   /* image won't fit OTA bank   */
#define OTA_PREPARE_BUSY         3   /* already in OTA mode        */
#define OTA_PREPARE_ERASE_FAIL   4   /* flash erase failed         */

/* How long to stay in OTA mode with no incoming DATA before giving up */
#define OTA_IDLE_TIMEOUT_MS  30000u

static uint16_t ota_rx_size   = 0;   /* expected image size                 */
static uint32_t ota_reboot_at = 0;   /* get_tick_ms() deadline to reset; 0=off */

static void ota_send_ack(PacketBuffer *txbuf, uint8_t op, uint8_t status, uint16_t off) {
    Packet ack;
    memset(&ack, 0, sizeof(Packet));
    ack.pOwner = NET;
    ack.pktDir = OUTGOING;
    ack.control_app = OTA_ACK;
    ack.data[0] = op;
    ack.data[1] = status;
    ack.data[2] = (uint8_t)(off & 0xFF);
    ack.data[3] = (uint8_t)(off >> 8);
    ack.length  = APP_HEADER_SIZE + 4;
    /* Queued only; the main loop's network/mac OUTGOING steps transmit it this
     * same iteration. END defers its reboot so this ACK leaves first. */
    if (buffer_full(txbuf) != GLOB_ERROR_BUFFER_FULL)
        write_packet(txbuf, &ack);
}

/* Returns true if pkt was an OTA control packet (handled — caller returns). */
static bool ota_rx_dispatch(Packet *pkt, PacketBuffer *txbuf) {
    uint8_t off = pkt->mesh_tbl_entries * 3;
    uint8_t data_len = (pkt->length > PACKET_HEADER_SIZE)
                       ? (uint8_t)(pkt->length - PACKET_HEADER_SIZE) : 0;
    uint8_t *p = &pkt->data[off];

    switch (pkt->control_app) {
    case OTA_PREPARE: {
        /* Handshake: TX announces version+size; RX checks, erases, enters OTA mode.
         * Payload: target(1) ver[16] size_lo size_hi crc[4]  = 23 bytes total */
        if (data_len < off + 23 || p[0] != get_mac_address()) return true;

        if (ota_state != OTA_STATE_IDLE || cfgota_state != CFGOTA_IDLE ||
            cfgota_verify_active) {
            ota_send_ack(txbuf, OTA_OP_PREPARE, OTA_PREPARE_BUSY, 0);
            return true;
        }

        /* Version check: reject if TX is pushing the build we're already running */
        char inc_ver[17];
        memcpy(inc_ver, &p[1], 16);
        inc_ver[16] = '\0';
        if (strncmp(inc_ver, fw_build_id_string(), 16) == 0) {
            print("[OTA] PREPARE: already on this version\n");
            ota_send_ack(txbuf, OTA_OP_PREPARE, OTA_PREPARE_SAME_VER, 0);
            return true;
        }

        uint16_t size = (uint16_t)p[17] | ((uint16_t)p[18] << 8);
        if (size == 0 || (uint32_t)size > ota_bank_size()) {
            ota_send_ack(txbuf, OTA_OP_PREPARE, OTA_PREPARE_TOO_LARGE, 0);
            return true;
        }

        if (ota_erase() != OTA_OK) {
            ota_send_ack(txbuf, OTA_OP_PREPARE, OTA_PREPARE_ERASE_FAIL, 0);
            return true;
        }

        ota_rx_size  = size;
        ota_state    = OTA_STATE_READY;
        ota_idle_dl  = get_tick_ms() + OTA_IDLE_TIMEOUT_MS;

        char b[72];
        snprintf(b, sizeof(b), "[OTA] PREPARE ok: ver=%s size=%u -> OTA mode\n",
                 inc_ver, size);
        print(b);
        ota_send_ack(txbuf, OTA_OP_PREPARE, OTA_PREPARE_OK, 0);
        return true;
    }
    case OTA_BEGIN: {
        /* Legacy path kept for backward compat — also transitions to READY state */
        if (data_len < off + 3 || p[0] != get_mac_address()) return true;
        if (ota_state != OTA_STATE_IDLE || cfgota_state != CFGOTA_IDLE ||
            cfgota_verify_active) {
            ota_send_ack(txbuf, OTA_OP_BEGIN, OTA_PREPARE_BUSY, 0);
            return true;
        }
        uint16_t size = (uint16_t)p[1] | ((uint16_t)p[2] << 8);
        uint8_t status = 1;
        if (size > 0 && (uint32_t)size <= ota_bank_size() && ota_erase() == OTA_OK) {
            ota_rx_size = size;
            ota_state   = OTA_STATE_READY;
            ota_idle_dl = get_tick_ms() + OTA_IDLE_TIMEOUT_MS;
            status = 0;
        }
        char b[48];
        snprintf(b, sizeof(b), "[OTA] begin size=%u %s\n", size, status ? "FAIL" : "ok");
        print(b);
        ota_send_ack(txbuf, OTA_OP_BEGIN, status, 0);
        return true;
    }
    case OTA_DATA: {
        if (data_len < off + 4 || p[0] != get_mac_address()) return true;
        uint16_t foff = (uint16_t)p[1] | ((uint16_t)p[2] << 8);
        uint8_t  flen = p[3];

        /* Reject DATA if we never agreed to an OTA session */
        if (ota_state == OTA_STATE_IDLE) {
            ota_send_ack(txbuf, OTA_OP_DATA, 0xFF, foff);
            return true;
        }

        /* Each DATA packet resets the idle timeout */
        ota_idle_dl  = get_tick_ms() + OTA_IDLE_TIMEOUT_MS;
        ota_state    = OTA_STATE_RECEIVING;

        uint8_t  status = 1;
        uint32_t limit  = (ota_rx_size + 7u) & ~7u;
        if (ota_rx_size > 0 && flen > 0 && (uint32_t)(foff + flen) <= limit &&
            (uint8_t)(data_len - off - 4) >= flen &&
            ota_write(foff, &p[4], flen) == OTA_OK) {
            status = 0;
        }
        ota_send_ack(txbuf, OTA_OP_DATA, status, foff);
        return true;
    }
    case OTA_END: {
        if (data_len < off + 7 || p[0] != get_mac_address()) return true;

        /* Reject END if not in an active OTA session */
        if (ota_state == OTA_STATE_IDLE) return true;

        uint16_t size = (uint16_t)p[1] | ((uint16_t)p[2] << 8);
        uint32_t want = (uint32_t)p[3] | ((uint32_t)p[4] << 8) |
                        ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 24);
        uint8_t status = 2;   /* default: CRC/size mismatch */
        if (size > 0 && size == ota_rx_size &&
            crc32_compute((const uint8_t *)ota_bank_addr(), size) == want) {
            ota_set_pending(size);
            ota_rx_size = 0;
            ota_state   = OTA_STATE_IDLE;
            status = 0;
            ota_reboot_at = get_tick_ms() + 800;   /* let the ACK transmit first */
            print("[OTA] end OK -> rebooting to apply\n");
        } else {
            /* CRC fail: stay in state so TX can retry end or abort */
            print("[OTA] end CRC FAIL\n");
        }
        ota_send_ack(txbuf, OTA_OP_END, status, 0);
        return true;
    }
    case OTA_ACK: {
        /* Observed on the gateway — surfaced for the host-side OTA driver. */
        if (data_len >= off + 4) {
            uint16_t aoff = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
            char b[64];
            snprintf(b, sizeof(b), "[OTA_ACK] from=%u op=%u status=%u off=%u\n",
                     pkt->source_adr, p[0], p[1], aoff);
            print(b);
        }
        return true;
    }
    default:
        return false;
    }
}

/* ---- OTA-over-LoRa CONFIGURATION (radio power / SF / beacon interval) ----
 *
 * Same handshake -> transfer -> ack-received -> apply -> restart shape as the
 * firmware OTA above, sized for a payload that fits in one packet instead of
 * needing chunked transfer:
 *   CFG_OTA_PREPARE: target(1) len(1)                          (handshake)
 *   CFG_OTA_DATA   : target(1) pwr(1) sf(1) beacon_lo beacon_hi (the values)
 *   CFG_OTA_END    : target(1) crc32[4]                        (commit trigger)
 *   CFG_OTA_ACK    : op(1) status(1)                           (target -> sender)
 *
 * Radio-parameter changes can't crash the firmware — they can only make the
 * node go deaf on the wrong SF/power, which nothing on the wire can detect
 * synchronously. So END does not mean "success": it stages the new values as
 * PROVISIONAL (new values + a full rollback backup + the mac of whichever
 * peer this node last heard from, all in one flash write), acks "applying",
 * and reboots. On the far side of that reboot the node pings its recorded
 * peer; a PONG commits (clears the pending flag), silence after
 * CFG_OTA_VERIFY_MAX_TRIES restores the backup — live, so the very rollback
 * ACK below can be heard — persists it, and reboots again into the known-good
 * state. Either outcome ends in one more CFG_OTA_ACK (RESULT) telling the
 * peer it verified or rolled back.
 */
#define CFG_OTA_OP_PREPARE 0
#define CFG_OTA_OP_DATA    1
#define CFG_OTA_OP_END     2
#define CFG_OTA_OP_RESULT  3   /* post-reboot outcome, sent to the verify peer */

#define CFG_OTA_STATUS_OK        0
#define CFG_OTA_STATUS_BUSY      1
#define CFG_OTA_STATUS_BAD_SIZE  2
#define CFG_OTA_STATUS_BAD_VALUE 3
#define CFG_OTA_STATUS_CRC_FAIL  4
#define CFG_OTA_STATUS_ROLLBACK  5   /* RESULT: link verify failed, reverted */
#define CFG_OTA_STATUS_VERIFIED  6   /* RESULT: link confirmed, committed    */

#define CFG_OTA_PAYLOAD_LEN        4u      /* pwr(1) sf(1) beacon_lo(1) beacon_hi(1) */
#define CFG_OTA_IDLE_TIMEOUT_MS    15000u  /* abort an unfinished transfer after this */
#define CFG_OTA_VERIFY_SETTLE_MS   2000u   /* let radio/timer come up before 1st ping */
#define CFG_OTA_VERIFY_TIMEOUT_MS  4000u   /* wait this long per attempt for a PONG   */
#define CFG_OTA_VERIFY_MAX_TRIES   5u

static uint8_t  cfgota_new_pwr           = 0;
static uint8_t  cfgota_new_sf            = 0;
static uint16_t cfgota_new_beacon        = 0;
static uint32_t cfgota_staged_crc        = 0;
static uint8_t  cfgota_verify_peer_pend  = 0;   /* captured at PREPARE, staged at END */
static uint32_t cfgota_reboot_at         = 0;   /* get_tick_ms() deadline; 0 = off     */

/* Boot-time link verification (armed in Reset_Handler when CFG_OTA_PENDING). */
static uint8_t  cfgota_verify_peer       = 0;
static uint8_t  cfgota_verify_tries      = 0;
static uint32_t cfgota_verify_next_at    = 0;
static bool     cfgota_verify_waiting    = false;   /* PING sent, awaiting PONG */

/* Most recent node this device heard a packet from — the default verify peer
 * for a config push, per the "ping whoever you last heard from" design: it
 * proves general mesh connectivity survived, not just that the original
 * pusher is still around. Updated at the very end of app_incoming so a
 * CFG_OTA_PREPARE handler sees the peer from BEFORE that same packet. */
static uint8_t last_rx_src = 0;

static void cfg_ota_send_ack(PacketBuffer *txbuf, uint8_t op, uint8_t status, uint8_t dest) {
    Packet ack;
    memset(&ack, 0, sizeof(Packet));
    ack.pOwner = NET;
    ack.pktDir = OUTGOING;
    ack.destination_adr = dest;   /* 0 = broadcast, seen by anyone listening */
    ack.control_app = CFG_OTA_ACK;
    ack.data[0] = op;
    ack.data[1] = status;
    ack.length  = APP_HEADER_SIZE + 2;
    if (buffer_full(txbuf) != GLOB_ERROR_BUFFER_FULL)
        write_packet(txbuf, &ack);
}

/* Returns true if pkt was a CFG-OTA control packet (handled — caller returns). */
static bool cfg_ota_rx_dispatch(Packet *pkt, PacketBuffer *txbuf) {
    uint8_t off = pkt->mesh_tbl_entries * 3;
    uint8_t data_len = (pkt->length > PACKET_HEADER_SIZE)
                       ? (uint8_t)(pkt->length - PACKET_HEADER_SIZE) : 0;
    uint8_t *p = &pkt->data[off];

    switch (pkt->control_app) {
    case CFG_OTA_PREPARE: {
        if (data_len < off + 2 || p[0] != get_mac_address()) return true;

        if (ota_state != OTA_STATE_IDLE || cfgota_state != CFGOTA_IDLE ||
            cfgota_verify_active) {
            cfg_ota_send_ack(txbuf, CFG_OTA_OP_PREPARE, CFG_OTA_STATUS_BUSY, 0);
            return true;
        }
        if (p[1] != CFG_OTA_PAYLOAD_LEN) {
            cfg_ota_send_ack(txbuf, CFG_OTA_OP_PREPARE, CFG_OTA_STATUS_BAD_SIZE, 0);
            return true;
        }

        cfgota_verify_peer_pend = last_rx_src ? last_rx_src : pkt->source_adr;
        cfgota_state   = CFGOTA_READY;
        cfgota_idle_dl = get_tick_ms() + CFG_OTA_IDLE_TIMEOUT_MS;
        print("[CFGOTA] PREPARE ok -> ready for config data\n");
        cfg_ota_send_ack(txbuf, CFG_OTA_OP_PREPARE, CFG_OTA_STATUS_OK, 0);
        return true;
    }
    case CFG_OTA_DATA: {
        if (data_len < off + 1 + CFG_OTA_PAYLOAD_LEN || p[0] != get_mac_address())
            return true;
        if (cfgota_state == CFGOTA_IDLE) {
            cfg_ota_send_ack(txbuf, CFG_OTA_OP_DATA, CFG_OTA_STATUS_BUSY, 0);
            return true;
        }

        uint8_t  pwr = p[1], sf = p[2];
        uint16_t beacon_s = (uint16_t)p[3] | ((uint16_t)p[4] << 8);
        if (pwr < 1 || pwr > 22 || sf < 5 || sf > 12 ||
            beacon_s < 1u || beacon_s > 255u) {
            cfgota_state = CFGOTA_IDLE;
            print("[CFGOTA] DATA rejected: value out of range\n");
            cfg_ota_send_ack(txbuf, CFG_OTA_OP_DATA, CFG_OTA_STATUS_BAD_VALUE, 0);
            return true;
        }

        cfgota_new_pwr    = pwr;
        cfgota_new_sf     = sf;
        cfgota_new_beacon = beacon_s;
        cfgota_staged_crc = crc32_compute(&p[1], CFG_OTA_PAYLOAD_LEN);
        cfgota_state      = CFGOTA_RECEIVED;
        cfgota_idle_dl    = get_tick_ms() + CFG_OTA_IDLE_TIMEOUT_MS;
        print("[CFGOTA] DATA ok -> all received, awaiting commit\n");
        cfg_ota_send_ack(txbuf, CFG_OTA_OP_DATA, CFG_OTA_STATUS_OK, 0);
        return true;
    }
    case CFG_OTA_END: {
        if (data_len < off + 5 || p[0] != get_mac_address()) return true;
        if (cfgota_state != CFGOTA_RECEIVED) {
            cfg_ota_send_ack(txbuf, CFG_OTA_OP_END, CFG_OTA_STATUS_BUSY, 0);
            return true;
        }

        uint32_t want;
        memcpy(&want, &p[1], 4);
        if (want != cfgota_staged_crc) {
            cfgota_state = CFGOTA_IDLE;
            print("[CFGOTA] END CRC FAIL — not applied\n");
            cfg_ota_send_ack(txbuf, CFG_OTA_OP_END, CFG_OTA_STATUS_CRC_FAIL, 0);
            return true;
        }

        /* Commit as PROVISIONAL: stage the new values + a full rollback
         * backup + the verify peer in ONE flash write. The live radio/RAM
         * state is untouched, so this ACK still goes out at the current
         * (proven-working) settings, exactly like OTA_END's reboot-after-ack. */
        addrEnum addrs[9]; uint8_t vals[9]; uint8_t n = 0;
        addrs[n] = RADIO_TX_PWR;         vals[n++] = cfgota_new_pwr;
        addrs[n] = RADIO_DR;             vals[n++] = cfgota_new_sf;
        addrs[n] = PARAMETER_N;          vals[n++] = (uint8_t)cfgota_new_beacon;
        addrs[n] = CFG_OTA_BACKUP_TXPWR; vals[n++] = radio_get_tx_power();
        addrs[n] = CFG_OTA_BACKUP_SF;    vals[n++] = radio_get_datarate();
        addrs[n] = CFG_OTA_BACKUP_BEACON;vals[n++] = (uint8_t)timer_get_beacon_interval_s();
        addrs[n] = CFG_OTA_VERIFY_PEER;  vals[n++] = cfgota_verify_peer_pend;
        addrs[n] = CFG_OTA_VERIFY_TRIES; vals[n++] = 0;
        addrs[n] = CFG_OTA_PENDING;      vals[n++] = 1;
        writeFlashParamBytes(addrs, vals, n);

        cfgota_state = CFGOTA_IDLE;
        char b[96];
        snprintf(b, sizeof(b),
                 "[CFGOTA] commit ok: pwr=%u sf=%u beacon=%u verify_peer=%u -> rebooting\n",
                 cfgota_new_pwr, cfgota_new_sf, cfgota_new_beacon, cfgota_verify_peer_pend);
        print(b);
        cfg_ota_send_ack(txbuf, CFG_OTA_OP_END, CFG_OTA_STATUS_OK, 0);
        cfgota_reboot_at = get_tick_ms() + 800;   /* let the ACK transmit first */
        return true;
    }
    case CFG_OTA_ACK: {
        /* Observed on the gateway/sender — surfaced for the host-side driver. */
        if (data_len >= off + 2) {
            char b[64];
            snprintf(b, sizeof(b), "[CFGOTA_ACK] from=%u op=%u status=%u\n",
                     pkt->source_adr, p[0], p[1]);
            print(b);
        }
        return true;
    }
    default:
        return false;
    }
}

static void app_incoming(Packet *pkt, PacketBuffer *txbuf) {
    /* OTA control traffic is high-volume (≈1000 fragments per image): handle it
     * before the verbose per-packet prints below so it neither spams the console
     * nor is slowed by USB-CDC output. */
    if (ota_rx_dispatch(pkt, txbuf))
        return;
    if (cfg_ota_rx_dispatch(pkt, txbuf))
        return;

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
        /* Print the piggybacked routing table this neighbour advertises so the
         * host can show each remote node's table (entries live at data[0..],
         * 3 bytes each: dest, next_hop, hop_count). */
        if (pkt->mesh_tbl_entries > 0) {
            char tb[224];
            int off = snprintf(tb, sizeof(tb), "[BTBL] src=%u n=%u",
                               pkt->source_adr, pkt->mesh_tbl_entries);
            for (uint8_t i = 0; i < pkt->mesh_tbl_entries; i++) {
                if (off >= (int)sizeof(tb) - 16)
                    break;
                off += snprintf(tb + off, sizeof(tb) - off, " %u>%u:%u",
                                pkt->data[i * 3], pkt->data[i * 3 + 1],
                                pkt->data[i * 3 + 2]);
            }
            snprintf(tb + off, sizeof(tb) - off, "\n");
            print(tb);
        }

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

    /* A PONG while a post-reboot CFG-OTA link check is outstanding is the
     * proof we needed: the new radio settings still reach the mesh. Commit
     * by clearing the pending flag and tell the verify peer it stuck. */
    if (pkt->control_app == PONG && cfgota_verify_active && cfgota_verify_waiting) {
        cfgota_verify_waiting = false;
        cfgota_verify_active  = false;
        writeFlashParamByte(CFG_OTA_PENDING, 0);
        cfg_ota_send_ack(txbuf, CFG_OTA_OP_RESULT, CFG_OTA_STATUS_VERIFIED, cfgota_verify_peer);
        print("[CFGOTA] link verified -> configuration committed\n");
    }

    /* Handle CMD_CFG on the field device: send CMD_ACK first, THEN apply
     * the new setting (so the ACK still goes out at the old power/SF and
     * is heard by the base). Beacon interval can optionally be persisted. */
    if (pkt->control_app == CMD_CFG) {
        uint8_t data_len = 0;
        if (pkt->length > PACKET_HEADER_SIZE)
            data_len = pkt->length - PACKET_HEADER_SIZE;
        uint8_t tbl_bytes = pkt->mesh_tbl_entries * 3;
        uint8_t off = tbl_bytes;

        uint8_t op = 0, new_pwr = radio_get_tx_power(), new_sf = radio_get_datarate();
        uint8_t target = BROADCAST, persist = 0;
        uint16_t new_beacon_s = timer_get_beacon_interval_s();
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
            } else if (op == 0x10 && data_len >= off + 5) {
                target = pkt->data[off + 1];
                persist = pkt->data[off + 2];
                new_beacon_s = (uint16_t)pkt->data[off + 3] |
                               ((uint16_t)pkt->data[off + 4] << 8);
                if ((target == BROADCAST || target == get_mac_address()) &&
                    persist <= 1u && new_beacon_s >= 1u && new_beacon_s <= 255u) {
                    status = 0;
                }
            }
        }

        if (op == 0x10 && target != BROADCAST && target != get_mac_address())
            return;

        /* Build CMD_ACK reply. Radio power/SF ACK before applying, because
         * changing those can make the ACK unreachable. Beacon interval does
         * not affect the radio link, and persistent flash writes block before
         * the queued packet can be serviced, so beacon ACK is sent after apply. */
        if (op != 0x10) {
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
        }

        if (status == 0) {
            if (op == 0x01 || op == 0x03) radio_set_tx_power(new_pwr);
            if (op == 0x02 || op == 0x03) radio_set_datarate(new_sf);
            if (op == 0x10) {
                timer_set_beacon_interval_s(new_beacon_s);
            }
            print("[CFG] applied\n");
        } else {
            print("[CFG] rejected (invalid value)\n");
        }

        if (op == 0x10) {
            Packet ack;
            memset(&ack, 0, sizeof(Packet));
            ack.pOwner = NET;
            ack.pktDir = OUTGOING;
            ack.control_app = CMD_ACK;
            ack.data[0] = op;
            ack.data[1] = status;
            ack.data[2] = target;
            ack.data[3] = persist;
            ack.data[4] = (uint8_t)(new_beacon_s & 0xFF);
            ack.data[5] = (uint8_t)(new_beacon_s >> 8);
            ack.length = 4 + 6;
            if (buffer_full(txbuf) != GLOB_ERROR_BUFFER_FULL) {
                write_packet(txbuf, &ack);
                network_interface(OUTGOING);
                mac_interface(OUTGOING);
            }
            if (status == 0 && persist)
                persist_beacon_interval_s((uint8_t)new_beacon_s);
        }
    }

    /* Print CMD_ACK on the base station so the operator sees confirmation. */
    if (pkt->control_app == CMD_ACK) {
        uint8_t data_len = 0;
        if (pkt->length > PACKET_HEADER_SIZE)
            data_len = pkt->length - PACKET_HEADER_SIZE;
        uint8_t tbl_bytes = pkt->mesh_tbl_entries * 3;
        uint8_t off = tbl_bytes;
        if (data_len >= off + 2) {
            char cbuf[128];
            uint8_t op = pkt->data[off];
            if (op == 0x10 && data_len >= off + 6) {
                uint16_t sec = (uint16_t)pkt->data[off+4] |
                               ((uint16_t)pkt->data[off+5] << 8);
                snprintf(cbuf, sizeof(cbuf),
                         "[CMD_ACK] from=%u op=%u status=%u target=%u persist=%u beacon_interval=%u\n",
                         pkt->source_adr, op, pkt->data[off+1],
                         pkt->data[off+2], pkt->data[off+3], sec);
                print(cbuf);
            } else if (data_len >= off + 4) {
                snprintf(cbuf, sizeof(cbuf),
                         "[CMD_ACK] from=%u op=%u status=%u tx_pwr=%u sf=%u\n",
                         pkt->source_adr,
                         pkt->data[off], pkt->data[off+1],
                         pkt->data[off+2], pkt->data[off+3]);
                print(cbuf);
            }
        }
    }

    /* Remember who we last heard from — the default CFG-OTA verify peer.
     * Updated last so a CFG_OTA_PREPARE handled above still sees the peer
     * from BEFORE this packet, not this packet's own sender. */
    if (pkt->source_adr != get_mac_address() && pkt->source_adr != 0)
        last_rx_src = pkt->source_adr;
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

    /* Copy .ramfunc into RAM (the OTA applier executes from here while it
     * erases/reprograms the application flash). Must run before any call into
     * ota_apply_at_boot(). */
    extern uint32_t _siramfunc, _sramfunc, _eramfunc;
    src = &_siramfunc; dst = &_sramfunc;
    while (dst < &_eramfunc) *dst++ = *src++;

    /* Apply a staged OTA image, if one is pending, before the watchdog starts.
     * On success this resets the MCU and never returns; otherwise it falls
     * through and the current application continues to boot.
     *
     * GATED OFF by default until the in-place applier is validated end-to-end
     * with fixed-on-fixed firmware (the flash-program fix is in place and the
     * scratch-page self-test passes — see `get ota selftest`). Build with
     * -DOTA_APPLY_ENABLED to enable. The OTA transfer + staging + pending flag
     * are fully functional regardless; only the auto-apply step is gated. */
#ifdef OTA_APPLY_ENABLED
    ota_apply_at_boot();
#else
    (void)ota_apply_at_boot;
#endif

    /* Update .noinit diagnostic state */
    uint8_t hung_chk = 0;   /* main-loop checkpoint from the boot that just died */
    if (diag.magic == DIAG_MAGIC) {
        diag.boot_count++;
        diag.last_init_stage = diag.init_stage;   /* how far we got last time */
        /* If the previous boot reached the main loop, main_chk holds the call
         * that was running when it hung (IWDG) or was interrupted (BOR/PIN). */
        if (diag.last_init_stage == 0xFFu)
            hung_chk = (uint8_t)diag.main_chk;
    } else {
        diag.magic = DIAG_MAGIC;
        diag.boot_count = 1;
        diag.last_init_stage = 0;
        diag.last_uptime_ms = 0;
        diag.main_chk = 0;
    }
    diag.last_reset_csr = csr_snapshot;
    diag.init_stage = 1;   /* entered Reset_Handler */
    diag.main_chk = 0;     /* not in main loop yet */

    /* Init hardware */
    clock_init();
    led_init();
    gpio_init();
    wdt_init();      /* Start IWDG immediately so any later hang is recoverable */
    diag.init_stage = 2;
    led1_on();

    timer_init();
    {
        uint8_t bint = 0xFF;
        readFlash(PARAMETER_N, &bint);
        if (bint >= 1u && bint <= 255u)
            timer_set_beacon_interval_s(bint);
    }
    diag.init_stage = 3;
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
                     diag.last_init_stage == 0xFFu,
                     hung_chk);
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

    /* Arm CFG-OTA link verification if a config push committed to flash as
     * PROVISIONAL just before this boot (see CFG_OTA_END in cfg_ota_rx_dispatch).
     * The actual ping/retry/rollback dance is driven from the main loop below
     * so it doesn't block boot or the watchdog. */
    {
        uint8_t pending = 0;
        readFlash(CFG_OTA_PENDING, &pending);
        if (pending == 1) {
            uint8_t peer = 0, tries = 0;
            readFlash(CFG_OTA_VERIFY_PEER, &peer);
            readFlash(CFG_OTA_VERIFY_TRIES, &tries);
            cfgota_verify_active  = true;
            cfgota_verify_peer    = peer;
            cfgota_verify_tries   = tries;
            cfgota_verify_next_at = get_tick_ms() + CFG_OTA_VERIFY_SETTLE_MS;
            char b[64];
            snprintf(b, sizeof(b),
                     "[CFGOTA] provisional config pending, verify_peer=%u tries=%u\n",
                     peer, tries);
            print(b);
        }
    }

    /* Main loop — fully polled, same structure as PIC24 */
    diag.init_stage = 0xFF;   /* reached main loop successfully */
    while (1) {
        diag.main_chk = MAINCHK_TOP;
        wdt_kick();

        /* OTA: reboot once the END-ACK has had time to transmit. The pending
         * image is applied early in Reset_Handler (see ota_apply_at_boot). */
        if (ota_reboot_at && get_tick_ms() >= ota_reboot_at) {
            SCB_AIRCR = 0x05FA0004u;   /* VECTKEY | SYSRESETREQ */
            while (1);
        }

        /* OTA idle timeout: if TX went silent after PREPARE/BEGIN, exit OTA mode */
        if (ota_state != OTA_STATE_IDLE && get_tick_ms() >= ota_idle_dl) {
            ota_state   = OTA_STATE_IDLE;
            ota_rx_size = 0;
            print("[OTA] idle timeout — resuming normal operation\n");
        }

        /* CFG-OTA: reboot once the END-ACK (commit) or the RESULT ack
         * (rollback) has had time to transmit. */
        if (cfgota_reboot_at && get_tick_ms() >= cfgota_reboot_at) {
            SCB_AIRCR = 0x05FA0004u;   /* VECTKEY | SYSRESETREQ */
            while (1);
        }

        /* CFG-OTA idle timeout: if TX went silent mid-handshake, abort back to idle */
        if (cfgota_state != CFGOTA_IDLE && get_tick_ms() >= cfgota_idle_dl) {
            cfgota_state = CFGOTA_IDLE;
            print("[CFGOTA] idle timeout — resuming normal operation\n");
        }

        /* CFG-OTA post-reboot link verification: ping the recorded peer;
         * a PONG (handled in app_incoming) commits. No PONG after
         * CFG_OTA_VERIFY_MAX_TRIES attempts means the new settings made us
         * deaf — restore the backup LIVE (so the rollback ack can be heard),
         * persist it, notify the peer, and reboot into the clean state. */
        if (cfgota_verify_active && radio_ok) {
            uint32_t now = get_tick_ms();
            if (cfgota_verify_waiting) {
                if (now >= cfgota_verify_next_at)
                    cfgota_verify_waiting = false;   /* this attempt timed out, retry below */
            } else if (now >= cfgota_verify_next_at) {
                if (cfgota_verify_tries >= CFG_OTA_VERIFY_MAX_TRIES) {
                    uint8_t bpwr = 14, bsf = 7, bbeacon = 10;
                    readFlash(CFG_OTA_BACKUP_TXPWR, &bpwr);
                    readFlash(CFG_OTA_BACKUP_SF, &bsf);
                    readFlash(CFG_OTA_BACKUP_BEACON, &bbeacon);
                    if (bpwr < 1 || bpwr > 22) bpwr = 14;
                    if (bsf < 5 || bsf > 12) bsf = 7;
                    if (bbeacon < 1) bbeacon = 10;

                    radio_set_tx_power(bpwr);
                    radio_set_datarate(bsf);
                    timer_set_beacon_interval_s(bbeacon);

                    addrEnum addrs[4]; uint8_t vals[4]; uint8_t n = 0;
                    addrs[n] = RADIO_TX_PWR;    vals[n++] = bpwr;
                    addrs[n] = RADIO_DR;        vals[n++] = bsf;
                    addrs[n] = PARAMETER_N;     vals[n++] = bbeacon;
                    addrs[n] = CFG_OTA_PENDING; vals[n++] = 0;
                    writeFlashParamBytes(addrs, vals, n);

                    cfg_ota_send_ack(&pTxBuf, CFG_OTA_OP_RESULT, CFG_OTA_STATUS_ROLLBACK,
                                     cfgota_verify_peer);
                    print("[CFGOTA] link verify failed -> rolled back to previous settings\n");
                    cfgota_verify_active = false;
                    cfgota_reboot_at = get_tick_ms() + 800;   /* let the ACK transmit first */
                } else {
                    Packet ping;
                    memset(&ping, 0, sizeof(Packet));
                    ping.pOwner = NET;
                    ping.pktDir = OUTGOING;
                    ping.control_app = PING;
                    ping.destination_adr = cfgota_verify_peer;
                    ping.length = 4;
                    if (buffer_full(&pTxBuf) != GLOB_ERROR_BUFFER_FULL) {
                        write_packet(&pTxBuf, &ping);
                        cfgota_verify_waiting = true;
                        cfgota_verify_tries++;
                        writeFlashParamByte(CFG_OTA_VERIFY_TRIES, cfgota_verify_tries);
                        cfgota_verify_next_at = now + CFG_OTA_VERIFY_TIMEOUT_MS;
                    }
                }
            }
        }

        diag.main_chk = MAINCHK_USB;   usb_cdc_poll();
        diag.main_chk = MAINCHK_TIMER; timer_poll();
        diag.main_chk = MAINCHK_GPS;   gps_poll();

        if (radio_ok) {
            diag.main_chk = MAINCHK_RADIO_IRQ; radio_irq_handler();

            diag.main_chk = MAINCHK_APP_IN;   dcf_app_interface(INCOMING, &pRxBuf, &pTxBuf);
            diag.main_chk = MAINCHK_APP_OUT;  dcf_app_interface(OUTGOING, &pRxBuf, &pTxBuf);
            diag.main_chk = MAINCHK_NET_IN;   network_interface(INCOMING);
            diag.main_chk = MAINCHK_NET_OUT;  network_interface(OUTGOING);
            diag.main_chk = MAINCHK_MAC_IN;   mac_interface(INCOMING);
            diag.main_chk = MAINCHK_MAC_OUT;  mac_interface(OUTGOING);
            diag.main_chk = MAINCHK_MAC_RETX; mac_interface(RETX);
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
