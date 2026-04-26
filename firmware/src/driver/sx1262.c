/*
 * sx1262.c — SX1262 radio driver for STM32U073
 * Ported from PIC24 sx1262.c
 *
 * Changes from PIC24:
 *   - SPI1_Exchange8bitBuffer() → spi_exchange_buffer()
 *   - RADIO_CSN  → GPIO PA4 (E22_NSS)
 *   - RADIO_BUSY → GPIO PB2 (E22_BUSY) input
 *   - RADIO_RESET → GPIO PB3 (E22_NRST)
 *   - RADIO_RXEN  → GPIO PA0
 *   - RADIO_TXEN  → GPIO PA1
 *   - ClrWdt() → removed (no watchdog)
 *   - readFlash/writeFlash → flash_config equivalents
 *   - LED_A_TOGGLE → led1_toggle()
 */

#include <string.h>
#include "radio.h"
#include "sx1262_register.h"
#include "spi.h"
#include "timer.h"
#include "flash_config.h"
#include "packetBuffer.h"
#include "stm32u0.h"
#include "hw_pins.h"

/* ---- GPIO helpers ---- */
#define CSN_LOW()     GPIO_BSRR(GPIOA_BASE) = (1 << (E22_NSS_PIN + 16))
#define CSN_HIGH()    GPIO_BSRR(GPIOA_BASE) = (1 << E22_NSS_PIN)
#define BUSY_READ()   ((GPIO_IDR(GPIOB_BASE) >> E22_BUSY_PIN) & 1)
#define NRST_LOW()    GPIO_BSRR(GPIOB_BASE) = (1 << (E22_NRST_PIN + 16))
#define NRST_HIGH()   GPIO_BSRR(GPIOB_BASE) = (1 << E22_NRST_PIN)
#define RXEN_SET(v)   do { if(v) GPIO_BSRR(GPIOA_BASE)=(1<<E22_RXEN_PIN); \
                          else  GPIO_BSRR(GPIOA_BASE)=(1<<(E22_RXEN_PIN+16)); } while(0)
#define TXEN_SET(v)   do { if(v) GPIO_BSRR(GPIOA_BASE)=(1<<E22_TXEN_PIN); \
                          else  GPIO_BSRR(GPIOA_BASE)=(1<<(E22_TXEN_PIN+16)); } while(0)

extern void led1_toggle(void);

/* ---- Local variables ---- */
static PacketBuffer *pktRxBuf;
static PacketBuffer *pktTxBuf;
static uint8_t tx_done_irq = 0;
static uint8_t radio_failed = 0;
static uint8_t current_sf = 7;
static uint8_t current_tx_power = 14;
static slme SLME;

/* Cached frequency for radio_get_channel */
static uint32_t current_freq_hz = 868000000;

/* ---- Local function declarations ---- */
static uint8_t reset_radio(void);
static uint8_t wait_busy(void);
static void write_command(uint8_t cmd, uint8_t *params, uint8_t len);
static uint8_t read_command(uint8_t cmd, uint8_t *params, uint8_t plen,
                            uint8_t *result, uint8_t rlen);
static void write_register(uint16_t addr, uint8_t *data, uint8_t len);
static void write_buffer(uint8_t offset, uint8_t *data, uint8_t len);
static void read_buffer(uint8_t offset, uint8_t *data, uint8_t len);
static void set_standby(uint8_t mode);
static void set_rx(uint32_t timeout);
static void set_tx(uint32_t timeout);
static void set_rf_frequency(uint32_t freq_hz);
static void set_modulation_params(uint8_t sf, uint8_t bw, uint8_t cr, uint8_t ldro);
static void set_packet_params(uint16_t preamble, uint8_t header_type,
                               uint8_t payload_len, uint8_t crc, uint8_t iq);
static void set_dio_irq_params(uint16_t irqMask, uint16_t dio1Mask,
                                uint16_t dio2Mask, uint16_t dio3Mask);
static void clear_irq_status(uint16_t mask);
static uint16_t get_irq_status(void);
static void set_tx_params(int8_t power, uint8_t rampTime);
static void set_pa_config(uint8_t dutyCycle, uint8_t hpMax, uint8_t devSel, uint8_t paLut);
static void receive_packet(void);
static void rxen_txen_set(uint8_t rx, uint8_t tx);

#define RSSI_THRESHOLD -100

/* ---- BUSY polling ---- */
static uint8_t wait_busy(void) {
    uint8_t outer = 25;
    if (radio_failed) return 1;
    while (outer--) {
        volatile uint16_t inner = 10000;
        while (BUSY_READ() && inner > 0)
            inner--;
        if (!BUSY_READ()) return 0;
    }
    radio_failed = 1;
    return 1;
}

/* ---- RXEN / TXEN control ---- */
static void rxen_txen_set(uint8_t rx, uint8_t tx) {
    RXEN_SET(rx);
    TXEN_SET(tx);
}

/* ---- SPI command write ---- */
static void write_command(uint8_t cmd, uint8_t *params, uint8_t len) {
    uint8_t buf[16];
    buf[0] = cmd;
    for (uint8_t i = 0; i < len && i < 15; i++)
        buf[i + 1] = params[i];
    if (wait_busy()) return;
    CSN_LOW();
    spi_exchange_buffer(buf, len + 1, buf);
    CSN_HIGH();
}

/* ---- SPI command read ---- */
static uint8_t read_command(uint8_t cmd, uint8_t *params, uint8_t plen,
                            uint8_t *result, uint8_t rlen) {
    uint8_t buf[16];
    if (wait_busy()) return 0;
    buf[0] = cmd;
    for (uint8_t i = 0; i < plen && i < 15; i++)
        buf[i + 1] = params[i];
    uint8_t total = 1 + plen + 1 + rlen;
    for (uint8_t i = 1 + plen; i < total; i++)
        buf[i] = 0x00;
    CSN_LOW();
    spi_exchange_buffer(buf, total, buf);
    CSN_HIGH();
    for (uint8_t i = 0; i < rlen; i++)
        result[i] = buf[1 + plen + 1 + i];
    return buf[1 + plen];
}

/* ---- Register write ---- */
static void write_register(uint16_t addr, uint8_t *data, uint8_t len) {
    uint8_t buf[16];
    buf[0] = SX1262_CMD_WRITE_REGISTER;
    buf[1] = (addr >> 8) & 0xFF;
    buf[2] = addr & 0xFF;
    for (uint8_t i = 0; i < len && i < 13; i++)
        buf[3 + i] = data[i];
    if (wait_busy()) return;
    CSN_LOW();
    spi_exchange_buffer(buf, 3 + len, buf);
    CSN_HIGH();
}

/* ---- Buffer write/read ---- */
static void write_buffer(uint8_t offset, uint8_t *data, uint8_t len) {
    uint8_t hdr[2];
    hdr[0] = SX1262_CMD_WRITE_BUFFER;
    hdr[1] = offset;
    if (wait_busy()) return;
    CSN_LOW();
    spi_exchange_buffer(hdr, 2, hdr);
    spi_exchange_buffer(data, len, data);
    CSN_HIGH();
}

static void read_buffer(uint8_t offset, uint8_t *data, uint8_t len) {
    uint8_t hdr[3];
    hdr[0] = SX1262_CMD_READ_BUFFER;
    hdr[1] = offset;
    hdr[2] = 0x00;
    if (wait_busy()) return;
    CSN_LOW();
    spi_exchange_buffer(hdr, 3, hdr);
    spi_exchange_buffer(data, len, data);
    CSN_HIGH();
}

/* ---- Mode commands ---- */
static void set_standby(uint8_t mode) {
    rxen_txen_set(0, 0);
    write_command(SX1262_CMD_SET_STANDBY, &mode, 1);
}

static void set_rx(uint32_t timeout) {
    uint8_t params[3];
    params[0] = (timeout >> 16) & 0xFF;
    params[1] = (timeout >> 8) & 0xFF;
    params[2] = timeout & 0xFF;
    rxen_txen_set(1, 0);
    write_command(SX1262_CMD_SET_RX, params, 3);
}

static void set_tx(uint32_t timeout) {
    uint8_t params[3];
    params[0] = (timeout >> 16) & 0xFF;
    params[1] = (timeout >> 8) & 0xFF;
    params[2] = timeout & 0xFF;
    rxen_txen_set(0, 1);
    write_command(SX1262_CMD_SET_TX, params, 3);
}

/* ---- Configuration helpers ---- */
static void set_rf_frequency(uint32_t freq_hz) {
    uint32_t mhz = freq_hz / 1000000UL;
    uint32_t rem = freq_hz % 1000000UL;
    uint32_t freq_reg = mhz * 1048576UL + (rem * 1048576UL + 500000UL) / 1000000UL;
    uint8_t params[4];
    params[0] = (freq_reg >> 24) & 0xFF;
    params[1] = (freq_reg >> 16) & 0xFF;
    params[2] = (freq_reg >> 8) & 0xFF;
    params[3] = freq_reg & 0xFF;
    write_command(SX1262_CMD_SET_RF_FREQUENCY, params, 4);
}

static void set_pa_config(uint8_t dutyCycle, uint8_t hpMax, uint8_t devSel, uint8_t paLut) {
    uint8_t params[4] = { dutyCycle, hpMax, devSel, paLut };
    write_command(SX1262_CMD_SET_PA_CONFIG, params, 4);
}

static void set_tx_params(int8_t power, uint8_t rampTime) {
    uint8_t params[2] = { (uint8_t)power, rampTime };
    write_command(SX1262_CMD_SET_TX_PARAMS, params, 2);
}

static void set_modulation_params(uint8_t sf, uint8_t bw, uint8_t cr, uint8_t ldro) {
    uint8_t params[4] = { sf, bw, cr, ldro };
    write_command(SX1262_CMD_SET_MODULATION_PARAMS, params, 4);
}

static void set_packet_params(uint16_t preamble, uint8_t header_type,
                               uint8_t payload_len, uint8_t crc, uint8_t iq) {
    uint8_t params[6];
    params[0] = (preamble >> 8) & 0xFF;
    params[1] = preamble & 0xFF;
    params[2] = header_type;
    params[3] = payload_len;
    params[4] = crc;
    params[5] = iq;
    write_command(SX1262_CMD_SET_PKT_PARAMS, params, 6);
}

static void set_dio_irq_params(uint16_t irqMask, uint16_t dio1Mask,
                                uint16_t dio2Mask, uint16_t dio3Mask) {
    uint8_t params[8];
    params[0] = (irqMask >> 8) & 0xFF;  params[1] = irqMask & 0xFF;
    params[2] = (dio1Mask >> 8) & 0xFF; params[3] = dio1Mask & 0xFF;
    params[4] = (dio2Mask >> 8) & 0xFF; params[5] = dio2Mask & 0xFF;
    params[6] = (dio3Mask >> 8) & 0xFF; params[7] = dio3Mask & 0xFF;
    write_command(SX1262_CMD_SET_DIO_IRQ_PARAMS, params, 8);
}

static void clear_irq_status(uint16_t mask) {
    uint8_t params[2] = { (mask >> 8) & 0xFF, mask & 0xFF };
    write_command(SX1262_CMD_CLR_IRQ_STATUS, params, 2);
}

static uint16_t get_irq_status(void) {
    uint8_t result[2] = {0};
    read_command(SX1262_CMD_GET_IRQ_STATUS, NULL, 0, result, 2);
    return ((uint16_t)result[0] << 8) | result[1];
}

/* ---- Reset ---- */
static uint8_t reset_radio(void) {
    NRST_HIGH();
    delay_ms(10);
    NRST_LOW();
    delay_ms(20);
    NRST_HIGH();
    delay_ms(10);
    return wait_busy();
}

/* ================================================================
 * PUBLIC HAL FUNCTIONS
 * ================================================================ */

uint8_t radio_init(PacketBuffer *pRxBuf, PacketBuffer *pTxBuf) {
    /* Configure radio GPIO pins */
    /* PA0 (RXEN), PA1 (TXEN) → output, push-pull */
    uint32_t moder = GPIO_MODER(GPIOA_BASE);
    moder &= ~((3 << (0*2)) | (3 << (1*2)));
    moder |=  ((1 << (0*2)) | (1 << (1*2)));
    GPIO_MODER(GPIOA_BASE) = moder;
    RXEN_SET(0);
    TXEN_SET(0);

    /* PB0 (DIO1), PB1 (DIO2) → input (default after reset) */
    /* PB2 (BUSY) → input */
    moder = GPIO_MODER(GPIOB_BASE);
    moder &= ~((3 << (0*2)) | (3 << (1*2)) | (3 << (2*2)));
    /* PB3 (NRST) → output */
    moder &= ~(3 << (3*2));
    moder |=  (1 << (3*2));
    GPIO_MODER(GPIOB_BASE) = moder;
    NRST_HIGH();

    if (!pRxBuf || !pTxBuf) return 1;
    pktRxBuf = pRxBuf;
    pktTxBuf = pTxBuf;

    if (reset_radio()) return 1;

    set_standby(SX1262_STANDBY_RC);

    /* TCXO on DIO3 (E22-900M22S uses TCXO, 1.7V, 5ms startup) */
    {
        uint8_t params[4] = { SX1262_TCXO_CTRL_1_7V, 0x00, 0x01, 0x40 };
        write_command(SX1262_CMD_SET_DIO3_AS_TCXO_CTRL, params, 4);
    }

    /* Calibrate all */
    {
        uint8_t cal = SX1262_CALIBRATE_ALL;
        write_command(SX1262_CMD_CALIBRATE, &cal, 1);
        delay_ms(10);
    }

    set_standby(SX1262_STANDBY_XOSC);

    /* DC-DC regulator */
    {
        uint8_t reg = SX1262_REGULATOR_DC_DC;
        write_command(SX1262_CMD_SET_REGULATOR_MODE, &reg, 1);
    }

    /* LoRa packet type */
    {
        uint8_t ptype = SX1262_PKT_TYPE_LORA;
        write_command(SX1262_CMD_SET_PKT_TYPE, &ptype, 1);
    }

    /* Load frequency from flash */
    uint8_t freq1, freq2, freq3, freq4;
    readFlash(RADIO_FREQ1, &freq1);
    readFlash(RADIO_FREQ2, &freq2);
    readFlash(RADIO_FREQ3, &freq3);
    readFlash(RADIO_FREQ4, &freq4);
    uint32_t stored_freq = ((uint32_t)freq1 << 24) | ((uint32_t)freq2 << 16) |
                           ((uint32_t)freq3 << 8) | (uint32_t)freq4;
    if (stored_freq != 444000000 && stored_freq != 868000000 && stored_freq != 870000000)
        stored_freq = 868000000;
    current_freq_hz = stored_freq;

    /* Calibrate image */
    {
        uint8_t cal_params[2];
        if (stored_freq > 860000000) { cal_params[0] = 0xD7; cal_params[1] = 0xDB; }
        else                         { cal_params[0] = 0x6B; cal_params[1] = 0x6F; }
        write_command(SX1262_CMD_CALIBRATE_IMAGE, cal_params, 2);
    }

    set_rf_frequency(stored_freq);

    /* PA config for +22dBm */
    set_pa_config(SX1262_PA_DUTY_CYCLE_22DBM, SX1262_PA_HP_MAX_22DBM,
                  SX1262_PA_DEVICE_SEL_SX1262, 0x01);

    /* TX power from flash */
    uint8_t stored_power;
    readFlash(RADIO_TX_PWR, &stored_power);
    if (stored_power < 1 || stored_power > 22) stored_power = 14;
    current_tx_power = stored_power;
    set_tx_params((int8_t)stored_power, SX1262_RAMP_200_US);

    /* Data rate from flash */
    uint8_t stored_dr;
    readFlash(RADIO_DR, &stored_dr);
    if (stored_dr < 5 || stored_dr > 12) stored_dr = 7;
    current_sf = stored_dr;

    uint8_t ldro = (current_sf >= 11) ? 1 : 0;
    set_modulation_params(current_sf, SX1262_LORA_BW_125, SX1262_LORA_CR_4_5, ldro);

    /* 8 preamble, explicit header, max 64 payload, CRC on, standard IQ */
    set_packet_params(8, SX1262_LORA_HEADER_EXPLICIT, 64,
                      SX1262_LORA_CRC_ON, SX1262_LORA_IQ_STANDARD);

    /* Buffer base: TX=0, RX=128 */
    { uint8_t p[2] = { 0x00, 0x80 }; write_command(SX1262_CMD_SET_BUFFER_BASE_ADDRESS, p, 2); }

    /* Private sync word 0x1424 */
    { uint8_t sw = SX1262_LORA_SYNC_WORD_PRIVATE_MSB; write_register(SX1262_REG_LORA_SYNC_WORD_MSB, &sw, 1); }
    { uint8_t sw = SX1262_LORA_SYNC_WORD_PRIVATE_LSB; write_register(SX1262_REG_LORA_SYNC_WORD_LSB, &sw, 1); }

    /* Boost RX gain */
    { uint8_t g = 0x96; write_register(SX1262_REG_RX_GAIN, &g, 1); }

    if (radio_failed) return 1;

    SLME.radio_initialized = TRUE;
    return 0;
}

static void receive_packet(void) {
    uint8_t payload[BUFFER_DATA_SIZE];

    uint8_t rx_status[2] = {0};
    read_command(SX1262_CMD_GET_RX_BUFFER_STATUS, NULL, 0, rx_status, 2);
    uint8_t receivedCount = rx_status[0];
    uint8_t rx_start = rx_status[1];

    if (receivedCount > (BUFFER_DATA_SIZE - 3) || receivedCount < 12) return;

    /* SX1262 GET_PKT_STATUS (datasheet 13.5.3):
     *   [0] RssiPkt        -> rssi  = -RssiPkt/2  dBm
     *   [1] SnrPkt (s8)    -> snr   =  SnrPkt/4   dB
     *   [2] SignalRssiPkt  -> prssi = -SignalRssiPkt/2 dBm */
    uint8_t pkt_status[3] = {0};
    read_command(SX1262_CMD_GET_PKT_STATUS, NULL, 0, pkt_status, 3);
    int16_t rssi  = -((int16_t)pkt_status[0]) / 2;
    int8_t  snr   =  ((int8_t)pkt_status[1])  / 4;
    int16_t prssi = -((int16_t)pkt_status[2]) / 2;

    memset(payload, 0, BUFFER_DATA_SIZE);
    read_buffer(rx_start, &payload[3], receivedCount);

    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));

    payload[2] = receivedCount;
    pkt.pktDir = INCOMING;
    pkt.pOwner = MAC;
    pkt.rssi = rssi;
    pkt.prssi = prssi;
    pkt.snr = snr;
    pkt.rxCnt = payload[2];
    pkt.destination_adr = payload[3];
    pkt.source_adr = payload[4];
    pkt.sequence_num = (payload[6] << 8) | payload[5];
    pkt.control_mac = payload[7];
    pkt.protocol_Ver = payload[8];
    pkt.TTL = payload[9];
    pkt.mesh_dest = payload[10];
    pkt.mesh_tbl_entries = payload[11];
    pkt.mesh_src = payload[12];
    pkt.control_app = payload[13];
    pkt.length = payload[14];

    if (pkt.mesh_tbl_entries > 16) pkt.mesh_tbl_entries = 0;
    if (pkt.length > receivedCount || pkt.length > 70) return;

    uint8_t data_size = pkt.length - PACKET_HEADER_SIZE;
    if (data_size < 50) {
        memcpy(pkt.data, &payload[15], data_size);
        if (GLOB_ERROR_BUFFER_FULL != buffer_full(pktRxBuf))
            write_packet(pktRxBuf, &pkt);
    }
}

void radio_irq_handler(void) {
    if (BUSY_READ()) return;

    uint16_t irqflags = get_irq_status();

    if (irqflags & SX1262_IRQ_RX_DONE) {
        if (!(irqflags & SX1262_IRQ_CRC_ERR))
            receive_packet();
        clear_irq_status(SX1262_IRQ_RX_DONE | SX1262_IRQ_CRC_ERR | SX1262_IRQ_HEADER_ERR);
    } else if (irqflags & SX1262_IRQ_TX_DONE) {
        tx_done_irq = 1;
        clear_irq_status(SX1262_IRQ_TX_DONE);
    }

    if (irqflags)
        clear_irq_status(SX1262_IRQ_ALL);
}

uint8_t radio_start_rx(void) {
    if (!SLME.radio_initialized) return 1;
    set_standby(SX1262_STANDBY_XOSC);
    set_dio_irq_params(SX1262_IRQ_RX_DONE | SX1262_IRQ_CRC_ERR,
                       SX1262_IRQ_RX_DONE | SX1262_IRQ_CRC_ERR, 0, 0);
    clear_irq_status(SX1262_IRQ_ALL);
    set_rx(SX1262_RX_CONTINUOUS);
    SLME.receiver_initialized = TRUE;
    return 0;
}

uint8_t radio_start_tx(void) {
    if (!SLME.radio_initialized) return 1;
    set_standby(SX1262_STANDBY_XOSC);
    set_dio_irq_params(SX1262_IRQ_TX_DONE, SX1262_IRQ_TX_DONE, 0, 0);
    clear_irq_status(SX1262_IRQ_ALL);
    SLME.sender_initialized = TRUE;
    return 0;
}

uint8_t radio_send(uint8_t *payload, uint8_t length) {
    radio_start_tx();
    write_buffer(0x00, payload, length);
    set_packet_params(8, SX1262_LORA_HEADER_EXPLICIT, length,
                      SX1262_LORA_CRC_ON, SX1262_LORA_IQ_STANDARD);

    tx_done_irq = 0;
    set_tx(0x09C400);  /* 10s timeout */

    uint16_t timeout = 10000;
    while (!tx_done_irq && timeout > 0) {
        uint16_t irq = get_irq_status();
        if (irq & SX1262_IRQ_TX_DONE) {
            tx_done_irq = 1;
            clear_irq_status(SX1262_IRQ_TX_DONE);
            break;
        }
        delay_ms(1);
        timeout--;
    }

    led1_toggle();
    radio_start_rx();
    return 0;
}

uint8_t radio_set_channel(uint32_t freq) {
    set_standby(SX1262_STANDBY_XOSC);
    uint8_t cal_params[2];
    if (freq > 860000000) { cal_params[0] = 0xD7; cal_params[1] = 0xDB; }
    else                   { cal_params[0] = 0x6B; cal_params[1] = 0x6F; }
    write_command(SX1262_CMD_CALIBRATE_IMAGE, cal_params, 2);
    set_rf_frequency(freq);
    current_freq_hz = freq;
    return radio_start_rx();
}

uint32_t radio_get_channel(void) {
    return current_freq_hz;
}

void radio_set_datarate(uint8_t data_rate) {
    if (data_rate < 5) data_rate = 5;
    if (data_rate > 12) data_rate = 12;
    current_sf = data_rate;
    uint8_t ldro = (current_sf >= 11) ? 1 : 0;
    set_modulation_params(current_sf, SX1262_LORA_BW_125, SX1262_LORA_CR_4_5, ldro);
}

uint8_t radio_get_datarate(void) { return current_sf; }

void radio_set_tx_power(uint8_t power) {
    if (power > 22) power = 22;
    current_tx_power = power;
    set_tx_params((int8_t)power, SX1262_RAMP_200_US);
}

uint8_t radio_get_tx_power(void) { return current_tx_power; }

uint8_t radio_get_carrier_detect_avg(void) {
    uint8_t result[1] = {0};
    read_command(SX1262_CMD_GET_RSSI_INST, NULL, 0, result, 1);
    int16_t rssi = -((int16_t)result[0]) / 2;
    return (rssi > RSSI_THRESHOLD) ? 1 : 0;
}

void radio_print_all_registers(void) {
    /* No-op on STM32 */
}
