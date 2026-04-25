/*
 * terminal.c — USB command parser and menu interface
 * Ported from PIC24 — replaced sprintf with snprintf, removed PIC24-specifics
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "terminal.h"
#include "stm32u0.h"
#include "radio.h"
#include "timer.h"
#include "hal.h"
#include "maclayer.h"
#include "networklayer.h"
#include "flash_config.h"
#include "packetBuffer.h"
#include "adc.h"
#include "ina219.h"
#include "i2c.h"
#include "flash_ota.h"

uint8_t sniffer = 0;

#define MAX_ARGV 128

static uint16_t parseArgs(uint8_t *str, uint8_t *argv[]);
static void help_command(void);
static void get_commands(uint16_t argc, uint8_t *argv[]);
static void send_commands(uint16_t argc, uint8_t *argv[]);
static uint8_t set_commands(uint16_t argc, uint8_t *argv[]);
static void ping_commands(void);
static void menu(uint16_t argc, uint8_t *argv[]);
static void set_frequency(uint32_t freq);
static void set_data_rate(uint8_t data_rate);
static void set_tx_power(uint8_t power);
static void set_macAddr(uint8_t mac_addr);
static void softwareReset(void);
static void jump_to_bootloader(void);

static deviceData_t flash_data;
static ulme Ulme;

static uint8_t beacon_flag_val = 1;
void send_beacon_flag(uint8_t flag) { beacon_flag_val = flag; }
uint8_t get_beacon_flag(void) { return beacon_flag_val; }

void terminal(uint8_t *msg, uint8_t size) {
    uint8_t cmdStr[MAX_ARGV];
    uint8_t *argv[MAX_ARGV];
    uint16_t argc;

    if (size >= MAX_ARGV) size = MAX_ARGV - 1;
    memcpy(cmdStr, msg, size);
    cmdStr[size] = '\0';
    argc = parseArgs(cmdStr, argv);
    menu(argc, argv);
}

void user_layer_init(PacketBuffer *pTxBuf) {
    Ulme.pktTxBuf = pTxBuf;
}

static uint16_t parseArgs(uint8_t *str, uint8_t *argv[]) {
    uint16_t argc = 0;
    uint8_t *ch = str;

    if (*ch == '\r' || *ch == '\n' || *ch == '\0') return 0;
    while (*ch == ' ') ch++;

    argv[argc++] = ch;
    while (*ch != ' ' && *ch != '\0' && *ch != '\r' && *ch != '\n') ch++;
    if (*ch == ' ' || *ch == '\r' || *ch == '\n') {
        *ch++ = '\0';
        while (*ch == ' ') ch++;
    }

    while (*ch != '\0' && *ch != '\r' && *ch != '\n') {
        argv[argc++] = ch;
        while (*ch != ' ' && *ch != '\0' && *ch != '\r' && *ch != '\n') ch++;
        if (*ch == ' ' || *ch == '\r' || *ch == '\n') {
            *ch++ = '\0';
            while (*ch == ' ' && *ch != '\0') ch++;
        }
    }
    return argc;
}

static void menu(uint16_t argc, uint8_t *argv[]) {
    char s[200];

    if (!argc) return;

    const char *cmd = (const char *)argv[0];

    if (strcmp(cmd, "help") == 0) {
        help_command();
    } else if (strcmp(cmd, "get") == 0) {
        get_commands(argc, argv);
    } else if (strcmp(cmd, "set") == 0) {
        set_commands(argc, argv);
    } else if (strcmp(cmd, "send") == 0) {
        send_commands(argc, argv);
    } else if (strcmp(cmd, "reset") == 0) {
        softwareReset();
    } else if (strcmp(cmd, "dfu") == 0) {
        jump_to_bootloader();
    } else if (strcmp(cmd, "version") == 0) {
        print("STM32_LORA_V1\n");
    } else if (strcmp(cmd, "ping") == 0) {
        ping_commands();
    } else {
        snprintf(s, sizeof(s), "Error: unknown cmd: %s\n", argv[0]);
        print(s);
        print("Type <help> for available commands\n");
    }
}

static void help_command(void) {
    print("Commands:\n"
          "  help - This message\n"
          "  get <param>\n"
          "  set <param> <value>\n"
          "  send <message>\n"
          "  ping\n"
          "  version\n"
          "  reset\n"
          "  dfu - Enter USB DFU bootloader\n"
          "Params: frequency, data_rate, tx_power, mac_address, flash, routing, battery, solar, charge\n");
}

static uint8_t set_commands(uint16_t argc, uint8_t *argv[]) {
    char s[200];

    if (argc >= 2 && strcmp((const char *)argv[1], "flash") == 0) {
        writeFlash(&flash_data);
        print("Done\n");
        return 0;
    }

    /* --- OTA commands --- */
    if (argc >= 2 && strcmp((const char *)argv[1], "ota") == 0) {
        if (argc >= 3 && strcmp((const char *)argv[2], "erase") == 0) {
            ota_err_t rc = ota_erase();
            print(rc == OTA_OK ? "OTA Erase: OK\r\n" : "OTA Erase: FAIL\r\n");
            return 0;
        }
        if (argc >= 5 && strcmp((const char *)argv[2], "write") == 0) {
            uint32_t offset = strtoul((const char *)argv[3], NULL, 16);
            const char *hex = (const char *)argv[4];
            if (strlen(hex) != 16) {
                print("OTA Write: FAIL err=-2\r\n");
                return 1;
            }
            uint8_t data[8];
            for (int i = 0; i < 8; i++) {
                char tmp[3] = { hex[i*2], hex[i*2+1], '\0' };
                data[i] = (uint8_t)strtoul(tmp, NULL, 16);
            }
            ota_err_t rc = ota_write(offset, data, 8);
            if (rc == OTA_OK) {
                print("OTA Write: OK\r\n");
            } else {
                char s[64];
                snprintf(s, sizeof(s), "OTA Write: FAIL err=%d\r\n", (int)rc);
                print(s);
            }
            return 0;
        }
        if (argc >= 4 && strcmp((const char *)argv[2], "pending") == 0) {
            uint16_t sz = (uint16_t)strtoul((const char *)argv[3], NULL, 10);
            ota_set_pending(sz);
            print("OTA Pending: SET\r\n");
            return 0;
        }
        if (argc >= 3 && strcmp((const char *)argv[2], "clear") == 0) {
            ota_clear_pending();
            print("OTA Pending: CLEARED\r\n");
            return 0;
        }
        print("Unknown OTA set command\n");
        return 1;
    }

    if (argc >= 3) {
        if (strcmp((const char *)argv[1], "frequency") == 0) {
            uint32_t freq = strtol((const char *)argv[2], NULL, 10);
            if (freq == 444000000 || freq == 868000000 || freq == 870000000) {
                set_frequency(freq);
                flash_data.radioFreq1 = (freq >> 24) & 0xFF;
                flash_data.radioFreq2 = (freq >> 16) & 0xFF;
                flash_data.radioFreq3 = (freq >> 8) & 0xFF;
                flash_data.radioFreq4 = freq & 0xFF;
            } else {
                snprintf(s, sizeof(s), "Error: frequency must be 444000000, 868000000 or 870000000\n");
                print(s);
                return 1;
            }
        } else if (strcmp((const char *)argv[1], "data_rate") == 0) {
            uint8_t dr = strtol((const char *)argv[2], NULL, 10);
            if (dr >= 5 && dr <= 12) {
                set_data_rate(dr);
                flash_data.radioDataRate = dr;
            } else {
                print("Error: data_rate must be 5-12\n");
                return 1;
            }
        } else if (strcmp((const char *)argv[1], "tx_power") == 0) {
            uint8_t pw = strtol((const char *)argv[2], NULL, 10);
            if (pw >= 1 && pw <= 22) {
                set_tx_power(pw);
                flash_data.radioTxPwr = pw;
            } else {
                print("Error: tx_power must be 1-22\n");
                return 1;
            }
        } else if (strcmp((const char *)argv[1], "mac_address") == 0) {
            uint8_t addr = strtol((const char *)argv[2], NULL, 10);
            set_macAddr(addr);
            flash_data.deviceID = addr;
        } else if (strcmp((const char *)argv[1], "beacon") == 0) {
            uint8_t b = strtol((const char *)argv[2], NULL, 10);
            if (b == 0 || b == 1) send_beacon_flag(b);
            print("Done\n");
        } else {
            print("Unknown parameter\n");
        }
    } else {
        print("Insufficient arguments\n");
    }
    return 0;
}

static void get_commands(uint16_t argc, uint8_t *argv[]) {
    char buf[128];

    if (argc <= 1) return;

    if (strcmp((const char *)argv[1], "frequency") == 0) {
        uint32_t freq = radio_get_channel();
        snprintf(buf, sizeof(buf), "Frequency: %lu Hz\n", (unsigned long)freq);
        print(buf);
    } else if (strcmp((const char *)argv[1], "data_rate") == 0) {
        snprintf(buf, sizeof(buf), "Data Rate: %u\n", radio_get_datarate());
        print(buf);
    } else if (strcmp((const char *)argv[1], "tx_power") == 0) {
        snprintf(buf, sizeof(buf), "TX Power: %u\n", radio_get_tx_power());
        print(buf);
    } else if (strcmp((const char *)argv[1], "mac_address") == 0) {
        snprintf(buf, sizeof(buf), "MAC Address: %u\n", get_mac_address());
        print(buf);
    } else if (strcmp((const char *)argv[1], "flash") == 0) {
        deviceData_t fData;
        uint8_t *p = (uint8_t *)&fData;
        const uint8_t ids[] = { DEVICE_ID, RADIO_FREQ1, RADIO_FREQ2, RADIO_FREQ3,
                                RADIO_FREQ4, RADIO_DR, RADIO_TX_PWR };
        for (int i = 0; i < (int)(sizeof(ids)/sizeof(ids[0])); i++)
            readFlash(ids[i], p + i);

        uint32_t freq = ((uint32_t)fData.radioFreq1 << 24) |
                        ((uint32_t)fData.radioFreq2 << 16) |
                        ((uint32_t)fData.radioFreq3 << 8) |
                        (uint32_t)fData.radioFreq4;
        snprintf(buf, sizeof(buf),
                 "\nFlash Data\nFrequency: %lu Hz\nData Rate: %u\n"
                 "TX Power: %u\nMAC Address: %u\n",
                 (unsigned long)freq, fData.radioDataRate,
                 fData.radioTxPwr, fData.deviceID);
        print(buf);
    } else if (strcmp((const char *)argv[1], "routing") == 0) {
        print_routing_table();
    } else if (strcmp((const char *)argv[1], "battery") == 0) {
        uint16_t raw = adc_read_battery_raw();
        uint32_t mv = ((uint32_t)raw * 6600UL) / 4096UL;
        snprintf(buf, sizeof(buf), "Battery: %lu mV (raw: %u)\n", (unsigned long)mv, raw);
        print(buf);
    } else if (strcmp((const char *)argv[1], "solar") == 0) {
        /* Full solar/charger readout from U10 INA219.
         * Shunt R = 0.05 Ω (R58, 50mΩ ±1%) → I[mA] = V_shunt[µV] / 50 */
        uint16_t bus_mv  = ina219_read_bus_mv();
        int32_t  sh_uv   = ina219_read_shunt_uv();
        int32_t  i_ma    = sh_uv / 50;
        int32_t  p_mw    = ((int32_t)bus_mv * i_ma) / 1000;
        ChargeStatus cs  = charge_get_status();
        snprintf(buf, sizeof(buf),
                 "Solar:  bus=%u mV  shunt=%ld µV  I=%ld mA  P=%ld mW  charge=%s\n",
                 bus_mv, (long)sh_uv, (long)i_ma, (long)p_mw, charge_status_str(cs));
        print(buf);
    } else if (strcmp((const char *)argv[1], "charge") == 0) {
        ChargeStatus s = charge_get_status();
        snprintf(buf, sizeof(buf), "Charge: %s\n", charge_status_str(s));
        print(buf);
    } else if (strcmp((const char *)argv[1], "vrefint") == 0) {
        /* Read 4 samples; report each + average to expose any glitch */
        uint16_t s[4];
        uint32_t sum = 0;
        for (int i = 0; i < 4; i++) { s[i] = adc_read_vrefint_raw(); sum += s[i]; }
        uint16_t avg = (uint16_t)(sum / 4);
        uint16_t vdda = adc_vdda_from_raw(avg);
        uint16_t cal = *(volatile uint16_t *)0x1FFF6E68u;
        snprintf(buf, sizeof(buf),
                 "VREFINT raws: %u %u %u %u  avg=%u  CAL=%u  VDDA=%u mV\n",
                 s[0], s[1], s[2], s[3], avg, cal, vdda);
        print(buf);
    } else if (strcmp((const char *)argv[1], "ina") == 0) {
        /* Write config register and report I2C result */
        int wret = i2c2_write_reg(0x40, 0x00, 0x399F);
        snprintf(buf, sizeof(buf), "INA219 config write: %d\n", wret);
        print(buf);
        uint16_t raw_shunt = 0, raw_bus = 0;
        int sr = i2c2_read_reg(0x40, 0x01, &raw_shunt);
        int br = i2c2_read_reg(0x40, 0x02, &raw_bus);
        snprintf(buf, sizeof(buf),
                 "shunt reg: ret=%d raw=0x%04X (%d mV)\n"
                 "bus   reg: ret=%d raw=0x%04X (%u mV)\n",
                 sr, raw_shunt, (int16_t)raw_shunt / 100,
                 br, raw_bus, (raw_bus >> 3) * 4);
        print(buf);
    } else if (strcmp((const char *)argv[1], "i2cscan") == 0) {
        print("Scanning I2C2 (0x08-0x77)...\n");
        for (uint8_t a = 0x08; a <= 0x77; a++) {
            uint8_t dummy;
            if (i2c2_read(a, &dummy, 1) == 0) {
                snprintf(buf, sizeof(buf), "  Found: 0x%02X\n", a);
                print(buf);
            }
        }
        print("Done\n");
    } else if (strcmp((const char *)argv[1], "adcscan") == 0) {
        print("ADC scan ch0-19:\n");
        for (uint8_t ch = 0; ch <= 19; ch++) {
            uint16_t raw = adc_read_channel_raw(ch);
            if (raw > 20) {
                uint32_t mv = ((uint32_t)raw * 3300UL) / 4096UL;
                snprintf(buf, sizeof(buf), "  CH%02u: raw=%4u  pin=%4lumV\n", ch, raw, (unsigned long)mv);
                print(buf);
            }
        }
        uint16_t bat = adc_read_battery_mv();
        uint16_t braw = adc_read_battery_raw();
        snprintf(buf, sizeof(buf), "bat_mv=%u (raw=%u)\n", bat, braw);
        print(buf);
    } else if (strcmp((const char *)argv[1], "ota") == 0) {
        if (argc >= 3 && strcmp((const char *)argv[2], "bank") == 0) {
            snprintf(buf, sizeof(buf), "OTA Bank: 0x%08lX size=%lu\r\n",
                     (unsigned long)ota_bank_addr(), (unsigned long)ota_bank_size());
            print(buf);
        } else if (argc >= 5 && strcmp((const char *)argv[2], "read") == 0) {
            uint32_t offset = strtoul((const char *)argv[3], NULL, 16);
            uint32_t len = strtoul((const char *)argv[4], NULL, 10);
            if (len > 16) len = 16;
            uint8_t tmp[16];
            ota_read(offset, tmp, len);
            print("OTA Read:");
            for (uint32_t i = 0; i < len; i++) {
                char hx[4];
                snprintf(hx, sizeof(hx), " %02X", tmp[i]);
                print(hx);
            }
            print("\r\n");
        } else if (argc >= 3 && strcmp((const char *)argv[2], "pending") == 0) {
            uint16_t img_sz = 0;
            if (ota_is_pending(&img_sz)) {
                snprintf(buf, sizeof(buf), "OTA Pending: YES size=%u\r\n", img_sz);
            } else {
                snprintf(buf, sizeof(buf), "OTA Pending: NO\r\n");
            }
            print(buf);
        }
    } else if (strcmp((const char *)argv[1], "i2cread") == 0 && argc >= 4) {
        /* Usage: get i2cread <addr_hex> <reg_hex>
         * Reads a 16-bit big-endian register from an arbitrary I2C device.
         * Use to prove I2C read path with a known-good device on H4. */
        uint8_t addr = (uint8_t)strtoul((const char *)argv[2], NULL, 16);
        uint8_t reg  = (uint8_t)strtoul((const char *)argv[3], NULL, 16);
        uint16_t val = 0;
        int r = i2c2_read_reg(addr, reg, &val);
        snprintf(buf, sizeof(buf),
                 "i2c read  addr=0x%02X reg=0x%02X -> ret=%d val=0x%04X\n",
                 addr, reg, r, val);
        print(buf);
    }
}

static void send_commands(uint16_t argc, uint8_t *argv[]) {
    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.pOwner = APP;
    pkt.pktDir = OUTGOING;

    uint8_t *dest = pkt.data;
    uint8_t remaining = sizeof(pkt.data) - 1;

    for (uint16_t i = 1; i < argc && remaining > 0; i++) {
        if (i > 1 && remaining > 0) { *dest++ = ' '; remaining--; }
        uint8_t *src = argv[i];
        while (*src && remaining > 0) { *dest++ = *src++; remaining--; }
    }
    pkt.length = dest - pkt.data;

    if (buffer_full(Ulme.pktTxBuf) != GLOB_ERROR_BUFFER_FULL)
        write_packet(Ulme.pktTxBuf, &pkt);
    else
        print("Buffer full\n");
}

static void ping_commands(void) {
    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.pOwner = NET;
    pkt.pktDir = OUTGOING;
    pkt.control_app = PING;
    pkt.length = 4;

    if (buffer_full(Ulme.pktTxBuf) != GLOB_ERROR_BUFFER_FULL)
        write_packet(Ulme.pktTxBuf, &pkt);
    else
        print("Buffer full\n");
}

void print_routing_table(void) {
    char buf[128];
    RoutingEntry *rt = get_routing_table();
    uint8_t n = get_routing_entries();

    snprintf(buf, sizeof(buf), "Routing table (%u entries):\n", n);
    print(buf);
    for (uint8_t i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf), "  [%u] dest=%u next=%u hops=%u age=%u\n",
                 i, rt[i].destination, rt[i].next_hop, rt[i].hop_count, rt[i].age);
        print(buf);
    }
}

static void set_frequency(uint32_t freq)   { radio_set_channel(freq); print("Done\n"); }
static void set_data_rate(uint8_t dr)      { radio_set_datarate(dr); print("Done\n"); }
static void set_tx_power(uint8_t pw)       { radio_set_tx_power(pw); print("Done\n"); }
static void set_macAddr(uint8_t addr) {
    if (set_mac_address(addr)) print("Error\n"); else print("Done\n");
}
static void softwareReset(void) {
    SCB_AIRCR = 0x05FA0004;  /* SYSRESETREQ */
    while (1);
}

#include "usb_cdc.h"

#define SYSTEM_MEMORY_ADDR  0x1FFF0000

static void jump_to_bootloader(void) {
    typedef void (*pFunction)(void);

    print("Entering DFU bootloader...\n");

    /* Give USB time to send the response */
    for (volatile int i = 0; i < 200000; i++);

    /* Disable USB pull-up so host sees disconnect */
    USB_BCDR &= ~USB_BCDR_DPPU;
    USB_CNTR = 0x0003;  /* FRES + PDWN — power down USB */

    /* Disable SysTick */
    SYST_CSR = 0;

    /* Disable all NVIC interrupts */
    NVIC_ICER = 0xFFFFFFFF;

    /* Clear any pending interrupts */
    NVIC_ISPR = 0;       /* write has no effect on pending; use ICPR */
    (*(volatile uint32_t *)0xE000E280) = 0xFFFFFFFF;  /* NVIC_ICPR */

    /* Enable SYSCFG clock and remap system memory to 0x00000000 */
    RCC_APBENR2 |= (1 << 0);  /* SYSCFGEN */
    SYSCFG_CFGR1 = (SYSCFG_CFGR1 & ~0x3) | 0x1;  /* MEM_MODE = 01 (system memory) */

    __asm volatile ("dsb");
    __asm volatile ("isb");

    /* Read reset vector from system memory */
    uint32_t bootloader_sp   = *(volatile uint32_t *)(SYSTEM_MEMORY_ADDR + 0);
    uint32_t bootloader_addr = *(volatile uint32_t *)(SYSTEM_MEMORY_ADDR + 4);

    /* Set main stack pointer and jump */
    __asm volatile ("msr msp, %0" :: "r" (bootloader_sp));
    ((pFunction)bootloader_addr)();

    while (1);
}
