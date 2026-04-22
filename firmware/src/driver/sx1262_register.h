/*
 * sx1262_register.h — SX1262 SPI command opcodes and register definitions
 * Copied verbatim from PIC24 project
 */

#ifndef SX1262_REGISTER_H
#define SX1262_REGISTER_H

/* ---- SPI Command Opcodes ---- */
#define SX1262_CMD_SET_SLEEP                0x84
#define SX1262_CMD_SET_STANDBY              0x80
#define SX1262_CMD_SET_FS                   0xC1
#define SX1262_CMD_SET_TX                   0x83
#define SX1262_CMD_SET_RX                   0x82
#define SX1262_CMD_STOP_TIMER_ON_PREAMBLE   0x9F
#define SX1262_CMD_SET_CAD                  0xC5
#define SX1262_CMD_SET_TX_CONTINUOUS_WAVE    0xD1
#define SX1262_CMD_SET_TX_INFINITE_PREAMBLE  0xD2
#define SX1262_CMD_SET_REGULATOR_MODE       0x96
#define SX1262_CMD_CALIBRATE                0x89
#define SX1262_CMD_CALIBRATE_IMAGE          0x98
#define SX1262_CMD_SET_PA_CONFIG            0x95
#define SX1262_CMD_SET_RX_TX_FALLBACK_MODE  0x93

#define SX1262_CMD_WRITE_REGISTER           0x0D
#define SX1262_CMD_READ_REGISTER            0x1D
#define SX1262_CMD_WRITE_BUFFER             0x0E
#define SX1262_CMD_READ_BUFFER              0x1E

#define SX1262_CMD_SET_DIO_IRQ_PARAMS       0x08
#define SX1262_CMD_GET_IRQ_STATUS           0x12
#define SX1262_CMD_CLR_IRQ_STATUS           0x02
#define SX1262_CMD_SET_DIO2_AS_RF_SWITCH    0x9D
#define SX1262_CMD_SET_DIO3_AS_TCXO_CTRL    0x97

#define SX1262_CMD_SET_RF_FREQUENCY         0x86
#define SX1262_CMD_SET_PKT_TYPE             0x8A
#define SX1262_CMD_GET_PKT_TYPE             0x11
#define SX1262_CMD_SET_TX_PARAMS            0x8E
#define SX1262_CMD_SET_MODULATION_PARAMS    0x8B
#define SX1262_CMD_SET_PKT_PARAMS           0x8C
#define SX1262_CMD_SET_BUFFER_BASE_ADDRESS  0x8F
#define SX1262_CMD_SET_LORA_SYMB_NUM_TIMEOUT 0xA0

#define SX1262_CMD_GET_STATUS               0xC0
#define SX1262_CMD_GET_RSSI_INST            0x15
#define SX1262_CMD_GET_RX_BUFFER_STATUS     0x13
#define SX1262_CMD_GET_PKT_STATUS           0x14
#define SX1262_CMD_GET_DEVICE_ERRORS        0x17
#define SX1262_CMD_CLR_DEVICE_ERRORS        0x07
#define SX1262_CMD_GET_STATS                0x10
#define SX1262_CMD_RESET_STATS              0x00

/* ---- Register Addresses ---- */
#define SX1262_REG_LORA_SYNC_WORD_MSB       0x0740
#define SX1262_REG_LORA_SYNC_WORD_LSB       0x0741
#define SX1262_REG_RX_GAIN                  0x08AC
#define SX1262_REG_OCP_CONFIGURATION        0x08E7
#define SX1262_REG_XTA_TRIM                 0x0911
#define SX1262_REG_XTB_TRIM                 0x0912

/* ---- IRQ Flags ---- */
#define SX1262_IRQ_TX_DONE                  0x0001
#define SX1262_IRQ_RX_DONE                  0x0002
#define SX1262_IRQ_PREAMBLE_DETECTED        0x0004
#define SX1262_IRQ_SYNC_WORD_VALID          0x0008
#define SX1262_IRQ_HEADER_VALID             0x0010
#define SX1262_IRQ_HEADER_ERR               0x0020
#define SX1262_IRQ_CRC_ERR                  0x0040
#define SX1262_IRQ_CAD_DONE                 0x0080
#define SX1262_IRQ_CAD_ACTIVITY_DETECTED    0x0100
#define SX1262_IRQ_RX_TX_TIMEOUT            0x0200
#define SX1262_IRQ_ALL                      0x03FF

/* ---- Standby modes ---- */
#define SX1262_STANDBY_RC                   0x00
#define SX1262_STANDBY_XOSC                 0x01

/* ---- Packet type ---- */
#define SX1262_PKT_TYPE_GFSK                0x00
#define SX1262_PKT_TYPE_LORA                0x01

/* ---- PA config ---- */
#define SX1262_PA_DUTY_CYCLE_22DBM          0x04
#define SX1262_PA_HP_MAX_22DBM              0x07
#define SX1262_PA_DEVICE_SEL_SX1262         0x00

/* ---- TX ramp time ---- */
#define SX1262_RAMP_10_US                   0x00
#define SX1262_RAMP_20_US                   0x01
#define SX1262_RAMP_40_US                   0x02
#define SX1262_RAMP_80_US                   0x03
#define SX1262_RAMP_200_US                  0x04
#define SX1262_RAMP_800_US                  0x05
#define SX1262_RAMP_1700_US                 0x06
#define SX1262_RAMP_3400_US                 0x07

/* ---- LoRa parameters ---- */
#define SX1262_LORA_BW_125                  0x04
#define SX1262_LORA_BW_250                  0x05
#define SX1262_LORA_BW_500                  0x06

#define SX1262_LORA_CR_4_5                  0x01
#define SX1262_LORA_CR_4_6                  0x02
#define SX1262_LORA_CR_4_7                  0x03
#define SX1262_LORA_CR_4_8                  0x04

#define SX1262_LORA_HEADER_EXPLICIT         0x00
#define SX1262_LORA_HEADER_IMPLICIT         0x01

#define SX1262_LORA_CRC_ON                  0x01
#define SX1262_LORA_CRC_OFF                 0x00

#define SX1262_LORA_IQ_STANDARD             0x00
#define SX1262_LORA_IQ_INVERTED             0x01

/* ---- Sync word ---- */
#define SX1262_LORA_SYNC_WORD_PRIVATE_MSB   0x14
#define SX1262_LORA_SYNC_WORD_PRIVATE_LSB   0x24

/* ---- TCXO voltage ---- */
#define SX1262_TCXO_CTRL_1_6V              0x00
#define SX1262_TCXO_CTRL_1_7V              0x01
#define SX1262_TCXO_CTRL_1_8V              0x02
#define SX1262_TCXO_CTRL_2_2V              0x03
#define SX1262_TCXO_CTRL_2_4V              0x04
#define SX1262_TCXO_CTRL_2_7V              0x05
#define SX1262_TCXO_CTRL_3_0V              0x06
#define SX1262_TCXO_CTRL_3_3V              0x07

/* ---- Calibration ---- */
#define SX1262_CALIBRATE_ALL                0x7F

/* ---- Regulator mode ---- */
#define SX1262_REGULATOR_LDO                0x00
#define SX1262_REGULATOR_DC_DC              0x01

/* ---- RX continuous ---- */
#define SX1262_RX_CONTINUOUS                0xFFFFFF

#endif /* SX1262_REGISTER_H */
