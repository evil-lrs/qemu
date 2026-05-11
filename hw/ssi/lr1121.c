/*
 * Minimal Semtech LR1121 SSI peripheral.
 *
 * Stub model: enough to let firmware probe the chip, configure it, write a
 * TX buffer, and issue SetTx. RX/RF/PHY are not modeled.
 *
 * LR1121 uses a 16-bit command opcode over SPI. BUSY is modeled as a simple
 * GPIO handshake, not cycle-accurate timing.
 *
 * Protocol reference: Semtech LR1121 User Manual Rev 2.1,
 * Host-Controller Interface and List of Commands.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/ssi/lr1121.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/irq.h"

#define LR1121_OP_GET_STATUS              0x0100
#define LR1121_OP_GET_VERSION             0x0101
#define LR1121_OP_WRITE_REG_MEM32         0x0105
#define LR1121_OP_READ_REG_MEM32          0x0106
#define LR1121_OP_WRITE_BUFFER8           0x0109
#define LR1121_OP_READ_BUFFER8            0x010A
#define LR1121_OP_CLEAR_RX_BUFFER         0x010B
#define LR1121_OP_WRITE_REG_MEM_MASK32    0x010C
#define LR1121_OP_GET_ERRORS              0x010D
#define LR1121_OP_CLEAR_ERRORS            0x010E
#define LR1121_OP_CALIBRATE               0x010F
#define LR1121_OP_SET_REG_MODE            0x0110
#define LR1121_OP_CALIB_IMAGE             0x0111
#define LR1121_OP_SET_DIO_AS_RF_SWITCH    0x0112
#define LR1121_OP_SET_DIO_IRQ_PARAMS      0x0113
#define LR1121_OP_CLEAR_IRQ               0x0114
#define LR1121_OP_CONFIG_LF_CLOCK         0x0116
#define LR1121_OP_SET_TCXO_MODE           0x0117
#define LR1121_OP_REBOOT                  0x0118
#define LR1121_OP_GET_VBAT                0x0119
#define LR1121_OP_GET_TEMP                0x011A
#define LR1121_OP_SET_SLEEP               0x011B
#define LR1121_OP_SET_STANDBY             0x011C
#define LR1121_OP_SET_FS                  0x011D
#define LR1121_OP_GET_RANDOM_NUMBER       0x0120
#define LR1121_OP_GET_CHIP_EUI            0x0125
#define LR1121_OP_GET_JOIN_EUI            0x0126
#define LR1121_OP_DERIVE_KEYS_PIN         0x0127
#define LR1121_OP_ENABLE_SPI_CRC          0x0128

#define LR1121_OP_RESET_STATS             0x0200
#define LR1121_OP_GET_STATS               0x0201
#define LR1121_OP_GET_PACKET_TYPE         0x0202
#define LR1121_OP_GET_RX_BUFFER_STATUS    0x0203
#define LR1121_OP_GET_PACKET_STATUS       0x0204
#define LR1121_OP_GET_RSSI_INST           0x0205
#define LR1121_OP_SET_GFSK_SYNC_WORD      0x0206
#define LR1121_OP_SET_LORA_PUBLIC_NET     0x0208
#define LR1121_OP_SET_RX                  0x0209
#define LR1121_OP_SET_TX                  0x020A
#define LR1121_OP_SET_RF_FREQUENCY        0x020B
#define LR1121_OP_AUTO_TX_RX              0x020C
#define LR1121_OP_SET_CAD_PARAMS          0x020D
#define LR1121_OP_SET_PACKET_TYPE         0x020E
#define LR1121_OP_SET_MODULATION_PARAMS   0x020F
#define LR1121_OP_SET_PACKET_PARAMS       0x0210
#define LR1121_OP_SET_TX_PARAMS           0x0211
#define LR1121_OP_SET_PACKET_ADRS         0x0212
#define LR1121_OP_SET_RX_TX_FALLBACK_MODE 0x0213
#define LR1121_OP_SET_RX_DUTY_CYCLE       0x0214
#define LR1121_OP_SET_PA_CONFIG           0x0215
#define LR1121_OP_STOP_TIMEOUT_PREAMBLE  0x0217
#define LR1121_OP_SET_CAD                 0x0218
#define LR1121_OP_SET_TX_CW               0x0219
#define LR1121_OP_SET_TX_INFINITE_PREAMBLE 0x021A
#define LR1121_OP_SET_LORA_SYNC_TIMEOUT   0x021B
#define LR1121_OP_SET_GFSK_CRC_PARAMS     0x0224
#define LR1121_OP_SET_GFSK_WHIT_PARAMS    0x0225
#define LR1121_OP_SET_RX_BOOSTED          0x0227
#define LR1121_OP_SET_RSSI_CALIBRATION    0x0229
#define LR1121_OP_SET_LORA_SYNC_WORD      0x022B
#define LR1121_OP_LRFHSS_BUILD_FRAME      0x022C
#define LR1121_OP_LRFHSS_SET_SYNC_WORD    0x022D
#define LR1121_OP_GET_LORA_RX_HDR_INFOS   0x0230

#define LR1121_OP_SET_GNSS_CONSTELLATION  0x0400
#define LR1121_OP_SET_GNSS_SCAN_MODE      0x0401

#define LR1121_OP_CRYPTO_SET_KEY          0x0502
#define LR1121_OP_CRYPTO_DERIVE_KEY       0x0503
#define LR1121_OP_CRYPTO_PROC_JOIN_ACC    0x0504
#define LR1121_OP_CRYPTO_COMP_AES_CMAC    0x0505
#define LR1121_OP_CRYPTO_VERIFY_AES_CMAC  0x0506
#define LR1121_OP_CRYPTO_AES_ENCRYPT_01   0x0507
#define LR1121_OP_CRYPTO_AES_ENCRYPT      0x0508
#define LR1121_OP_CRYPTO_AES_DECRYPT      0x0509
#define LR1121_OP_CRYPTO_STORE_FLASH      0x050A
#define LR1121_OP_CRYPTO_RESTORE_FLASH    0x050B
#define LR1121_OP_CRYPTO_SET_PARAM        0x050D
#define LR1121_OP_CRYPTO_GET_PARAM        0x050E

#define LR1121_IRQ_TX_DONE       0x00000004u
#define LR1121_IRQ_RX_DONE       0x00000008u
#define LR1121_IRQ_TIMEOUT       0x00000400u
#define LR1121_IRQ_HEADER_ERROR  0x00000040u
#define LR1121_IRQ_CRC_ERROR     0x00000080u
#define LR1121_IRQ_CMD_ERROR     0x00400000u

/* Stat1: [7:4] RFU, [3:1] Command Status, [0] Interrupt Status */
#define LR1121_STAT1_CMD_FAIL     0x00
#define LR1121_STAT1_CMD_PERR     0x01
#define LR1121_STAT1_CMD_OK       0x02
#define LR1121_STAT1_CMD_DAT      0x03

/* Stat2: [7:4] Reset Status, [3:1] Chip Mode, [0] Bootloader */
#define LR1121_STAT2_MODE_SLEEP      0x00
#define LR1121_STAT2_MODE_STDBY_RC   0x01
#define LR1121_STAT2_MODE_STDBY_XOSC 0x02
#define LR1121_STAT2_MODE_FS         0x03
#define LR1121_STAT2_MODE_RX         0x04
#define LR1121_STAT2_MODE_TX         0x05

#define LR1121_STAT2_BOOT_FLASH      0x01

static void lr1121_complete_opcode(LR1121State *s);

static const char *lr1121_opcode_name(uint16_t op)
{
    switch (op) {
    case LR1121_OP_GET_STATUS:            return "GetStatus";
    case LR1121_OP_GET_VERSION:           return "GetVersion";
    case LR1121_OP_WRITE_REG_MEM32:       return "WriteRegMem32";
    case LR1121_OP_READ_REG_MEM32:        return "ReadRegMem32";
    case LR1121_OP_WRITE_BUFFER8:         return "WriteBuffer8";
    case LR1121_OP_READ_BUFFER8:          return "ReadBuffer8";
    case LR1121_OP_CLEAR_RX_BUFFER:       return "ClearRxBuffer";
    case LR1121_OP_GET_ERRORS:            return "GetErrors";
    case LR1121_OP_CLEAR_ERRORS:          return "ClearErrors";
    case LR1121_OP_SET_DIO_IRQ_PARAMS:    return "SetDioIrqParams";
    case LR1121_OP_CLEAR_IRQ:             return "ClearIrq";
    case LR1121_OP_SET_SLEEP:             return "SetSleep";
    case LR1121_OP_SET_STANDBY:           return "SetStandby";
    case LR1121_OP_SET_FS:                return "SetFs";
    case LR1121_OP_GET_PACKET_TYPE:       return "GetPacketType";
    case LR1121_OP_GET_RX_BUFFER_STATUS:  return "GetRxBufferStatus";
    case LR1121_OP_GET_PACKET_STATUS:     return "GetPacketStatus";
    case LR1121_OP_SET_RX:                return "SetRx";
    case LR1121_OP_SET_TX:                return "SetTx";
    case LR1121_OP_SET_RF_FREQUENCY:      return "SetRfFrequency";
    case LR1121_OP_SET_PACKET_TYPE:       return "SetPacketType";
    case LR1121_OP_SET_MODULATION_PARAMS: return "SetModulationParams";
    case LR1121_OP_SET_PACKET_PARAMS:     return "SetPacketParams";
    case LR1121_OP_SET_TX_PARAMS:         return "SetTxParams";
    case LR1121_OP_SET_PA_CONFIG:         return "SetPaConfig";
    case LR1121_OP_SET_RX_TX_FALLBACK_MODE: return "SetRxTxFallbackMode";
    case LR1121_OP_GET_VBAT:              return "GetVbat";
    case LR1121_OP_GET_TEMP:              return "GetTemp";
    case LR1121_OP_GET_CHIP_EUI:          return "GetChipEui";
    case LR1121_OP_GET_JOIN_EUI:          return "GetJoinEui";
    case LR1121_OP_DERIVE_KEYS_PIN:       return "DeriveKeysAndGetPin";
    case LR1121_OP_RESET_STATS:           return "ResetStats";
    case LR1121_OP_GET_STATS:             return "GetStats";
    case LR1121_OP_GET_RSSI_INST:         return "GetRssiInst";
    case LR1121_OP_SET_GFSK_SYNC_WORD:    return "SetGfskSyncWord";
    case LR1121_OP_SET_LORA_PUBLIC_NET:   return "SetLoRaPublicNetwork";
    case LR1121_OP_SET_LORA_SYNC_WORD:    return "SetLoRaSyncWord";
    case LR1121_OP_SET_CAD:               return "SetCad";
    case LR1121_OP_SET_CAD_PARAMS:        return "SetCadParams";
    default:                              return NULL;
    }
}


static uint16_t lr1121_param_count(uint16_t op)
{
    switch (op) {
    case LR1121_OP_GET_STATUS:
    case LR1121_OP_GET_VERSION:
    case LR1121_OP_GET_ERRORS:
    case LR1121_OP_CLEAR_ERRORS:
    case LR1121_OP_CLEAR_RX_BUFFER:
    case LR1121_OP_SET_FS:
    case LR1121_OP_GET_RANDOM_NUMBER:
    case LR1121_OP_GET_PACKET_TYPE:
    case LR1121_OP_GET_RX_BUFFER_STATUS:
    case LR1121_OP_GET_PACKET_STATUS:
    case LR1121_OP_GET_RSSI_INST:
    case LR1121_OP_GET_VBAT:
    case LR1121_OP_GET_TEMP:
    case LR1121_OP_GET_CHIP_EUI:
    case LR1121_OP_GET_JOIN_EUI:
    case LR1121_OP_DERIVE_KEYS_PIN:
    case LR1121_OP_RESET_STATS:
    case LR1121_OP_GET_STATS:
    case LR1121_OP_SET_CAD:
    case LR1121_OP_SET_TX_CW:
    case LR1121_OP_SET_TX_INFINITE_PREAMBLE:
    case LR1121_OP_GET_LORA_RX_HDR_INFOS:
        return 0;

    case LR1121_OP_CALIBRATE:
    case LR1121_OP_SET_REG_MODE:
    case LR1121_OP_SET_SLEEP:
    case LR1121_OP_SET_STANDBY:
    case LR1121_OP_SET_PACKET_TYPE:
    case LR1121_OP_SET_RX_TX_FALLBACK_MODE:
    case LR1121_OP_CONFIG_LF_CLOCK:
    case LR1121_OP_REBOOT:
    case LR1121_OP_SET_LORA_PUBLIC_NET:
    case LR1121_OP_SET_LORA_SYNC_WORD:
    case LR1121_OP_ENABLE_SPI_CRC:
    case LR1121_OP_STOP_TIMEOUT_PREAMBLE:
    case LR1121_OP_SET_LORA_SYNC_TIMEOUT:
    case LR1121_OP_SET_RX_BOOSTED:
        return 1;

    case LR1121_OP_SET_TX_PARAMS:
    case LR1121_OP_CALIB_IMAGE:
    case LR1121_OP_SET_PACKET_ADRS:
    case LR1121_OP_SET_GFSK_WHIT_PARAMS:
        return 2;

    case LR1121_OP_READ_BUFFER8:
        return 2; /* offset, len */

    case LR1121_OP_SET_RX:
    case LR1121_OP_SET_TX:
        return 3; /* timeout */

    case LR1121_OP_SET_RF_FREQUENCY:
    case LR1121_OP_CLEAR_IRQ:
    case LR1121_OP_SET_TCXO_MODE:
    case LR1121_OP_LRFHSS_SET_SYNC_WORD:
        return 4;

    case LR1121_OP_READ_REG_MEM32:
        return 5; /* addr(4), len(1) */

    case LR1121_OP_AUTO_TX_RX:
    case LR1121_OP_SET_CAD_PARAMS:
    case LR1121_OP_SET_RX_DUTY_CYCLE:
        return 7;

    case LR1121_OP_SET_DIO_IRQ_PARAMS:
        return 8; /* Irq1ToEnable(4), Irq2ToEnable(4) */

    case LR1121_OP_SET_DIO_AS_RF_SWITCH:
    case LR1121_OP_SET_GFSK_SYNC_WORD:
    case LR1121_OP_SET_GFSK_CRC_PARAMS:
        return 8;

    case LR1121_OP_SET_PACKET_PARAMS:
        return 9;

    case LR1121_OP_SET_MODULATION_PARAMS:
        return 10; /* Fixed size for LoRa/GFSK */

    case LR1121_OP_SET_RSSI_CALIBRATION:
        return 11;

    case LR1121_OP_WRITE_REG_MEM_MASK32:
        return 12; /* addr(4), mask(4), data(4) */

    case LR1121_OP_WRITE_REG_MEM32:
        return 4; /* addr(4), then data until CS deassert */

    case LR1121_OP_WRITE_BUFFER8:
    case LR1121_OP_LRFHSS_BUILD_FRAME:
        return 0; /* data until CS deassert */


    default:
        return 0;
    }
}

static uint8_t lr1121_status_byte_for_index(LR1121State *s, uint16_t i)
{
    uint8_t stat1 = s->status1;
    /* Update dynamic interrupt bit */
    if (s->irq_status != 0) {
        stat1 |= 0x01;
    } else {
        stat1 &= ~0x01;
    }

    switch (i) {
    case 0: return stat1;
    case 1: return s->status2;
    case 2: return (s->irq_status >> 24) & 0xff;
    case 3: return (s->irq_status >> 16) & 0xff;
    case 4: return (s->irq_status >> 8) & 0xff;
    case 5: return s->irq_status & 0xff;
    default: return 0;
    }
}

static void lr1121_update_irq(LR1121State *s)
{
    bool levels[LR1121_DIO_COUNT] = {
        (s->irq_status & s->irq_dio1) != 0,
        (s->irq_status & s->irq_dio2) != 0,
        (s->irq_status & s->irq_dio3) != 0,
    };

    qemu_log_mask(LOG_GUEST_ERROR, "LR1121[SPI%d:CS%d]: update_irq status=0x%08x dio1_mask=0x%08x levels=[%d %d %d]\n",
                  s->spi_id, s->parent_obj.cs_index, s->irq_status, s->irq_dio1,
                  levels[0], levels[1], levels[2]);

    for (int i = 0; i < LR1121_DIO_COUNT; i++) {
        if (s->dio_level[i] != levels[i]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "LR1121[SPI%d:CS%d]: DIO%d %s irq_status=0x%08x\n",
                          s->spi_id, s->parent_obj.cs_index, i + 1,
                          levels[i] ? "high" : "low",
                          s->irq_status);
            s->dio_level[i] = levels[i];
        }
        qemu_set_irq(s->dio[i], levels[i]);
    }
}
#define LR1121_STAT2_RESET_CLEARED   0x00
#define LR1121_STAT2_RESET_ANALOG    0x01

#define LR1121_STAT2_BOOT_FLASH      0x01

static void lr1121_load_defaults(LR1121State *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->buf, 0, sizeof(s->buf));
    memset(s->param_buf, 0, sizeof(s->param_buf));
    memset(s->packet_params, 0, sizeof(s->packet_params));
    memset(s->modulation_params, 0, sizeof(s->modulation_params));

    s->selected = false;
    s->phase = LR1121_STATE_IDLE;
    s->opcode = 0;
    s->last_read_opcode = 0;
    s->param_pos = 0;
    s->param_needed = 0;
    s->response_pos = 0;

    s->status1 = (LR1121_STAT1_CMD_OK << 1);
    s->status2 = (LR1121_STAT2_RESET_ANALOG << 4) | (LR1121_STAT2_MODE_STDBY_RC << 1) | LR1121_STAT2_BOOT_FLASH;
    s->irq_status = 0;
    s->irq_dio1 = 0;

    s->irq_dio2 = 0;
    s->irq_dio3 = 0;
    s->errors = 0;

    s->rf_freq_hz = 0;
    s->packet_type = 0;
    s->tx_payload_len = 0;
    s->rx_payload_len = 0;
    s->rx_start_offset = 0;

    s->lora_syncword = 0x12;
    s->gfsk_syncword = 0x9723522556536564ULL;
    s->lora_public_network = 0;
    s->fallback_mode = 0x01; /* STBY_RC */

    memset(s->dio_level, 0, sizeof(s->dio_level));
    s->busy_level = false;

    lr1121_update_irq(s);
}

static void lr1121_set_busy(LR1121State *s, bool level)
{
    if (s->busy_level != level) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "LR1121[SPI%d:CS%d]: BUSY %s\n",
                      s->spi_id, s->parent_obj.cs_index,
                      level ? "high" : "low");
        s->busy_level = level;
    }
    qemu_set_irq(s->busy, level);
}

static int lr1121_set_cs(SSIPeripheral *ss, bool select)
{
    LR1121State *s = LR1121(ss);
    bool is_selected = (select == (ss->spc->cs_polarity == SSI_CS_HIGH));

    qemu_log_mask(LOG_GUEST_ERROR,
                  "LR1121[SPI%d:CS%d]: set_cs level=%d selected=%d phase=%d\n",
                  s->spi_id, s->parent_obj.cs_index, select, is_selected, s->phase);

    if (is_selected && !s->selected) {
        s->phase = LR1121_STATE_IDLE;
        s->opcode = 0;
        /* Do not reset last_read_opcode here */
        s->param_pos = 0;
        s->param_needed = 0;
        s->response_pos = 0;
        lr1121_set_busy(s, true);
    }

    if (!is_selected && s->selected) {
        if ((s->opcode == LR1121_OP_WRITE_BUFFER8 ||
             s->opcode == LR1121_OP_LRFHSS_BUILD_FRAME) &&
            s->phase == LR1121_STATE_BUF_DATA) {
            s->tx_payload_len = s->buf_addr;
        } else if (s->phase == LR1121_STATE_PARAMS || s->phase == LR1121_STATE_OPCODE_LSB) {
            lr1121_complete_opcode(s);
        }

        /* If it was a read command, keep it as last_read_opcode */
        if (s->opcode != 0x0000 && s->opcode != 0) {
             const char *name = lr1121_opcode_name(s->opcode);
             if (name && (g_str_has_prefix(name, "Get") || g_str_has_prefix(name, "Read"))) {
                 s->last_read_opcode = s->opcode;
             } else {
                 s->last_read_opcode = 0;
             }
        }

        lr1121_set_busy(s, false);
        s->phase = LR1121_STATE_IDLE;
    }

    s->selected = is_selected;
    return 0;
}

static void lr1121_send_state(LR1121State *s)
{
    SemtechRadioFrame f = {
        .chip = "lr1121",
        .radio_id = s->radio_id,
        .freq_hz = s->rf_freq_hz,
        .packet_type = s->packet_type == 0x02 ? "lora" : "fsk",
    };

    GString *out = g_string_new("");
    semtech_frame_to_state_json(&f, true, out);
    semtech_air_bus_send_json(&s->air_bus, out->str);
    g_string_free(out, true);
}

static void lr1121_log_tx_payload(LR1121State *s)
{
    SemtechRadioFrame f = {
        .chip = "lr1121",
        .radio_id = s->radio_id,
        .freq_hz = s->rf_freq_hz,
        .packet_type = s->packet_type == 0x02 ? "lora" : "fsk",
        .payload_len = s->tx_payload_len,
    };
    memcpy(f.payload, s->buf, MIN(s->tx_payload_len, sizeof(f.payload)));

    GString *js = g_string_new("");
    semtech_frame_to_tx_json(&f, js);

    if (!semtech_air_bus_send_json(&s->air_bus, js->str)) {
        semtech_frame_log_tx(&f, s->tx_log_path);
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "LR1121[SPI%d:CS%d]: TX len=%u freq_hz=%u "
                  "pkt_type=0x%02x\n",
                  s->spi_id,
                  s->parent_obj.cs_index,
                  s->tx_payload_len,
                  s->rf_freq_hz,
                  s->packet_type);
    
    g_string_free(js, true);
}

static void lr1121_complete_opcode(LR1121State *s)
{
    const char *name = lr1121_opcode_name(s->opcode);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "LR1121[SPI%d:CS%d]: opcode 0x%04x (%s) complete params=%u\n",
                  s->spi_id,
                  s->parent_obj.cs_index,
                  s->opcode,
                  name ? name : "?",
                  s->param_pos);

    switch (s->opcode) {
    case LR1121_OP_GET_STATUS:
        s->status2 &= 0x0F; /* Clear Reset Status (bits 7:4) */
        break;

    case LR1121_OP_SET_RF_FREQUENCY:
        s->rf_freq_hz =
            ((uint32_t)s->param_buf[0] << 24) |
            ((uint32_t)s->param_buf[1] << 16) |
            ((uint32_t)s->param_buf[2] << 8)  |
            ((uint32_t)s->param_buf[3]);
        lr1121_send_state(s);
        break;

    case LR1121_OP_SET_PACKET_TYPE:
        s->packet_type = s->param_buf[0];
        lr1121_send_state(s);
        break;

    case LR1121_OP_SET_MODULATION_PARAMS:
        s->modulation_params_len = MIN((size_t)s->param_pos,
                                       sizeof(s->modulation_params));
        memcpy(s->modulation_params, s->param_buf, s->modulation_params_len);
        break;

    case LR1121_OP_SET_PACKET_PARAMS:
        s->packet_params_len = MIN((size_t)s->param_pos,
                                   sizeof(s->packet_params));
        memcpy(s->packet_params, s->param_buf, s->packet_params_len);

        if (s->param_pos >= 6) {
            s->tx_payload_len = s->param_buf[5];
        }
        break;

    case LR1121_OP_SET_TX_PARAMS:
        s->tx_params[0] = s->param_buf[0];
        s->tx_params[1] = s->param_buf[1];
        break;

    case LR1121_OP_SET_DIO_IRQ_PARAMS:
        s->irq_dio1 =
            ((uint32_t)s->param_buf[0] << 24) |
            ((uint32_t)s->param_buf[1] << 16) |
            ((uint32_t)s->param_buf[2] << 8)  |
            ((uint32_t)s->param_buf[3]);
        s->irq_dio2 =
            ((uint32_t)s->param_buf[4] << 24) |
            ((uint32_t)s->param_buf[5] << 16) |
            ((uint32_t)s->param_buf[6] << 8)  |
            ((uint32_t)s->param_buf[7]);
        s->irq_dio3 = 0;
        qemu_log_mask(LOG_GUEST_ERROR, "LR1121[SPI%d:CS%d]: SetDioIrqParams irq1=0x%08x irq2=0x%08x\n",
                      s->spi_id, s->parent_obj.cs_index, s->irq_dio1, s->irq_dio2);
        break;

    case LR1121_OP_CLEAR_IRQ: {
        uint32_t mask =
            ((uint32_t)s->param_buf[0] << 24) |
            ((uint32_t)s->param_buf[1] << 16) |
            ((uint32_t)s->param_buf[2] << 8)  |
            ((uint32_t)s->param_buf[3]);
        s->irq_status &= ~mask;
        qemu_log_mask(LOG_GUEST_ERROR, "LR1121[SPI%d:CS%d]: ClearIrq mask=0x%08x\n",
                      s->spi_id, s->parent_obj.cs_index, mask);
        break;
    }

    case LR1121_OP_CLEAR_ERRORS:
        s->errors = 0;
        break;

    case LR1121_OP_SET_PA_CONFIG:
        /* Stub: PA config not modeled, just store if needed */
        break;

    case LR1121_OP_SET_TCXO_MODE:
        /* Stub: TCXO not modeled */
        break;

    case LR1121_OP_CALIBRATE:
    case LR1121_OP_CALIB_IMAGE:
        /* Stub: Calibration returns to Standby RC */
        s->status2 = (s->status2 & 0xF1) | (LR1121_STAT2_MODE_STDBY_RC << 1);
        break;

    case LR1121_OP_READ_REG_MEM32:
        s->mem_addr =
            ((uint32_t)s->param_buf[0] << 24) |
            ((uint32_t)s->param_buf[1] << 16) |
            ((uint32_t)s->param_buf[2] << 8)  |
            ((uint32_t)s->param_buf[3]);
        s->read_len = s->param_buf[4] * 4;
        break;

    case LR1121_OP_WRITE_REG_MEM32:
        s->mem_addr =
            ((uint32_t)s->param_buf[0] << 24) |
            ((uint32_t)s->param_buf[1] << 16) |
            ((uint32_t)s->param_buf[2] << 8)  |
            ((uint32_t)s->param_buf[3]);
        break;

    case LR1121_OP_WRITE_REG_MEM_MASK32: {
        uint32_t addr =
            ((uint32_t)s->param_buf[0] << 24) |
            ((uint32_t)s->param_buf[1] << 16) |
            ((uint32_t)s->param_buf[2] << 8)  |
            ((uint32_t)s->param_buf[3]);
        uint32_t mask =
            ((uint32_t)s->param_buf[4] << 24) |
            ((uint32_t)s->param_buf[5] << 16) |
            ((uint32_t)s->param_buf[6] << 8)  |
            ((uint32_t)s->param_buf[7]);
        uint32_t data =
            ((uint32_t)s->param_buf[8] << 24) |
            ((uint32_t)s->param_buf[9] << 16) |
            ((uint32_t)s->param_buf[10] << 8)  |
            ((uint32_t)s->param_buf[11]);
        
        if (addr + 3 < LR1121_REG_BYTES) {
            uint32_t val = (uint32_t)s->regs[addr] << 24 |
                           (uint32_t)s->regs[addr + 1] << 16 |
                           (uint32_t)s->regs[addr + 2] << 8 |
                           (uint32_t)s->regs[addr + 3];
            val = (val & ~mask) | (data & mask);
            s->regs[addr] = (val >> 24) & 0xff;
            s->regs[addr + 1] = (val >> 16) & 0xff;
            s->regs[addr + 2] = (val >> 8) & 0xff;
            s->regs[addr + 3] = val & 0xff;
        }
        break;
    }

    case LR1121_OP_SET_STANDBY:
        s->status2 = (s->status2 & 0xF1) | (s->param_buf[0] == 0x00 ?
            (LR1121_STAT2_MODE_STDBY_RC << 1) : (LR1121_STAT2_MODE_STDBY_XOSC << 1));
        break;

    case LR1121_OP_SET_FS:
        s->status2 = (s->status2 & 0xF1) | (LR1121_STAT2_MODE_FS << 1);
        break;

    case LR1121_OP_SET_TX:
        lr1121_log_tx_payload(s);
        s->irq_status |= LR1121_IRQ_TX_DONE;
        s->status2 = (s->status2 & 0xF1) | (LR1121_STAT2_MODE_TX << 1);
        break;

    case LR1121_OP_SET_RX:
        s->status2 = (s->status2 & 0xF1) | (LR1121_STAT2_MODE_RX << 1);
        break;

    case LR1121_OP_SET_LORA_PUBLIC_NET:
        s->lora_public_network = s->param_buf[0];
        s->lora_syncword = s->lora_public_network ? 0x34 : 0x12;
        break;

    case LR1121_OP_SET_LORA_SYNC_WORD:
        s->lora_syncword = s->param_buf[0];
        break;

    case LR1121_OP_SET_GFSK_SYNC_WORD:
        s->gfsk_syncword =
            ((uint64_t)s->param_buf[0] << 56) |
            ((uint64_t)s->param_buf[1] << 48) |
            ((uint64_t)s->param_buf[2] << 40) |
            ((uint64_t)s->param_buf[3] << 32) |
            ((uint64_t)s->param_buf[4] << 24) |
            ((uint64_t)s->param_buf[5] << 16) |
            ((uint64_t)s->param_buf[6] << 8)  |
            ((uint64_t)s->param_buf[7]);
        break;

    case LR1121_OP_SET_RX_TX_FALLBACK_MODE:
        s->fallback_mode = s->param_buf[0];
        break;

    case LR1121_OP_RESET_STATS:
        /* Stub: stats not modeled */
        break;

    case LR1121_OP_SET_CAD:
        s->irq_status |= 0x00000100u; /* CAD_DONE */
        s->status2 = (s->status2 & 0xF1) | (LR1121_STAT2_MODE_STDBY_RC << 1);
        break;

    case LR1121_OP_REBOOT:
        lr1121_load_defaults(s);
        break;

    default:
        break;
    }

    s->status1 = (s->status1 & 0xF1) | (LR1121_STAT1_CMD_OK << 1);
    lr1121_update_irq(s);
}

static uint8_t lr1121_response_byte(LR1121State *s)
{
    uint16_t i = s->response_pos++;
    uint16_t op = s->opcode;
    uint8_t res = 0;

    if (op == 0x0000 && s->last_read_opcode != 0) {
        op = s->last_read_opcode;
    }

    switch (op) {
    case LR1121_OP_GET_STATUS:
        if (i == 0) res = s->status1;
        else if (i == 1) res = s->status2;
        else if (i == 2) res = (s->irq_status >> 24) & 0xff;
        else if (i == 3) res = (s->irq_status >> 16) & 0xff;
        else if (i == 4) res = (s->irq_status >> 8) & 0xff;
        else if (i == 5) res = s->irq_status & 0xff;
        break;

    case LR1121_OP_GET_VERSION:
        if (i == 0) res = s->status1;
        else if (i == 1) res = 0x01; /* hw */
        else if (i == 2) res = 0x03; /* type: LR1121 */
        else if (i == 3) res = 0x01; /* fw major */
        else if (i == 4) res = 0x01; /* fw minor */
        break;

    case LR1121_OP_GET_ERRORS:
        if (i == 0) res = s->status1;
        else if (i == 1) res = (s->errors >> 24) & 0xff;
        else if (i == 2) res = (s->errors >> 16) & 0xff;
        else if (i == 3) res = (s->errors >> 8) & 0xff;
        else if (i == 4) res = s->errors & 0xff;
        break;

    case LR1121_OP_GET_PACKET_TYPE:
        if (i == 0) res = s->status1;
        else if (i == 1) res = s->packet_type;
        break;

    case LR1121_OP_GET_RX_BUFFER_STATUS:
        if (i == 0) res = s->status1;
        else if (i == 1) res = s->rx_payload_len;
        else if (i == 2) res = s->rx_start_offset;
        break;

    case LR1121_OP_GET_PACKET_STATUS:
        if (i == 0) res = s->status1;
        else if (s->packet_type == 0x02) { /* LoRa */
            if (i == 1) res = 100; /* RssiPkt: -50 dBm */
            else if (i == 2) res = 40;  /* SnrPkt: +10 dB */
            else if (i == 3) res = 100; /* SignalRssiPkt: -50 dBm */
        } else if (s->packet_type == 0x01) { /* GFSK */
            if (i == 1) res = 100; /* RssiSync */
            else if (i == 2) res = 100; /* RssiAvg */
            else if (i == 3) res = s->rx_payload_len; /* RxLen */
            else if (i == 4) res = 0x02; /* PktRcvd = 1 */
        }
        break;

    case LR1121_OP_READ_BUFFER8:
        if (i == 0) res = s->status1;
        else if ((i - 1) < s->read_len) {
            res = s->buf[(uint8_t)(s->buf_addr + (i - 1))];
        }
        break;

    case LR1121_OP_READ_REG_MEM32:
        if (i == 0) res = s->status1;
        else if ((i - 1) < s->read_len) {
            uint32_t addr = s->mem_addr + (i - 1);
            res = addr < LR1121_REG_BYTES ? s->regs[addr] : 0;
        }
        break;

    case LR1121_OP_GET_RANDOM_NUMBER:
        if (i == 0) res = s->status1;
        else if (i == 1) res = 0x12;
        else if (i == 2) res = 0x34;
        else if (i == 3) res = 0x56;
        else if (i == 4) res = 0x78;
        break;

    case LR1121_OP_GET_VBAT:
        if (i == 0) res = s->status1;
        else if (i == 1) res = 0xB6; /* ~3.45V */
        break;

    case LR1121_OP_GET_TEMP:
        if (i == 0) res = s->status1;
        else if (i == 1) res = 0x00;
        else if (i == 2) res = 0x00;
        break;

    case LR1121_OP_GET_CHIP_EUI:
        if (i == 0) res = s->status1;
        else if (i >= 1 && i <= 8) res = 0x11 * i; /* Dummy EUI */
        break;

    case LR1121_OP_GET_JOIN_EUI:
        if (i == 0) res = s->status1;
        else if (i >= 1 && i <= 8) res = 0x22 * i; /* Dummy EUI */
        break;

    case LR1121_OP_DERIVE_KEYS_PIN:
        if (i == 0) res = s->status1;
        else if (i == 1) res = 0x12;
        else if (i == 2) res = 0x34;
        else if (i == 3) res = 0x56;
        else if (i == 4) res = 0x78;
        break;

    case LR1121_OP_GET_STATS:
        if (i == 0) res = s->status1;
        break;

    case LR1121_OP_GET_RSSI_INST:
        if (i == 0) res = s->status1;
        else if (i == 1) res = 180; /* -90 dBm */
        break;

    case LR1121_OP_GET_LORA_RX_HDR_INFOS:
        if (i == 0) res = s->status1;
        else if (i == 1) res = 0x11;
        break;

    default:
        res = lr1121_status_byte_for_index(s, i);
        break;
    }

    qemu_log_mask(LOG_GUEST_ERROR, "LR1121[SPI%d:CS%d]: response_byte op=0x%04x i=%d res=0x%02x\n",
                  s->spi_id, s->parent_obj.cs_index, op, i, res);
    return res;
}

static uint32_t lr1121_transfer(SSIPeripheral *ss, uint32_t tx)
{
    LR1121State *s = LR1121(ss);
    uint8_t in = tx & 0xff;
    uint8_t out = lr1121_status_byte_for_index(s, s->response_pos);
    const char *name;

    if (!s->selected) {
        return 0;
    }

    qemu_log_mask(LOG_GUEST_ERROR, "LR1121[SPI%d:CS%d]: transfer in=0x%02x out=0x%02x phase=%d pos=%d\n",
                  s->spi_id, s->parent_obj.cs_index, in, out, s->phase, s->param_pos);

    switch (s->phase) {
    case LR1121_STATE_IDLE:
        if (in == 0x00) {
            /* 1-byte dummy read/NOP */
            s->opcode = 0x0000;
            s->param_pos = 0;
            s->param_needed = 0;
            s->response_pos = 1; /* Skip status1, next byte will be status2 or first data byte */
            s->phase = LR1121_STATE_DRAIN;
            return s->status1;
        }
        s->opcode = ((uint16_t)in) << 8;
        s->param_pos = 0;
        s->param_needed = 0;
        s->response_pos = 1;
        s->phase = LR1121_STATE_OPCODE_LSB;
        return s->status1;

    case LR1121_STATE_OPCODE_LSB:
        s->opcode |= in;
        s->param_needed = lr1121_param_count(s->opcode);
        s->param_pos = 0;
        s->response_pos = 2;

        name = lr1121_opcode_name(s->opcode);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "LR1121[SPI%d:CS%d]: opcode 0x%04x (%s) need=%u params\n",
                      s->spi_id,
                      s->parent_obj.cs_index,
                      s->opcode,
                      name ? name : "?",
                      s->param_needed);

        if (s->opcode == LR1121_OP_WRITE_BUFFER8 ||
            s->opcode == LR1121_OP_LRFHSS_BUILD_FRAME) {
            s->buf_addr = 0;
            s->tx_payload_len = 0;
            s->phase = LR1121_STATE_BUF_DATA;
            return s->status2;
        }

        if (s->param_needed == 0) {
            lr1121_complete_opcode(s);
            s->phase = LR1121_STATE_DRAIN;
            s->response_pos = 0;
            return s->status2;
        }

        s->phase = LR1121_STATE_PARAMS;
        return s->status2;

    case LR1121_STATE_PARAMS:
        out = lr1121_status_byte_for_index(s, s->response_pos++);

        if (s->param_pos < sizeof(s->param_buf)) {
            s->param_buf[s->param_pos] = in;
        }
        s->param_pos++;

        if (s->param_pos >= s->param_needed) {
            lr1121_complete_opcode(s);

            switch (s->opcode) {
            case LR1121_OP_WRITE_REG_MEM32:
                s->phase = LR1121_STATE_MEM_DATA;
                break;

            case LR1121_OP_READ_REG_MEM32:
            case LR1121_OP_READ_BUFFER8:
            case LR1121_OP_GET_STATUS:
            case LR1121_OP_GET_VERSION:
            case LR1121_OP_GET_ERRORS:
            case LR1121_OP_GET_PACKET_TYPE:
            case LR1121_OP_GET_RX_BUFFER_STATUS:
            case LR1121_OP_GET_PACKET_STATUS:
            case LR1121_OP_GET_RANDOM_NUMBER:
                s->phase = LR1121_STATE_DRAIN;
                s->response_pos = 0;
                break;

            default:
                s->phase = LR1121_STATE_DRAIN;
                break;
            }
        }

        return out;

    case LR1121_STATE_BUF_DATA:
        if (s->buf_addr < LR1121_BUF_BYTES) {
            s->buf[s->buf_addr++] = in;
            s->tx_payload_len = s->buf_addr;
        }
        return lr1121_status_byte_for_index(s, s->response_pos++);

    case LR1121_STATE_MEM_DATA:
        if (s->mem_addr < LR1121_REG_BYTES) {
            s->regs[s->mem_addr++] = in;
        }
        return lr1121_status_byte_for_index(s, s->response_pos++);

    case LR1121_STATE_DRAIN:
        return lr1121_response_byte(s);

    default:
        return s->status1;
    }
}

static void lr1121_realize(SSIPeripheral *ss, Error **errp)
{
    LR1121State *s = LR1121(ss);

    if (!s->radio_id) {
        s->radio_id = g_strdup_printf("lr1121-%d", s->parent_obj.cs_index);
    }

    semtech_air_bus_init(&s->air_bus, NULL, s);
    semtech_air_bus_start(&s->air_bus);

    lr1121_load_defaults(s);
}

static void lr1121_reset(DeviceState *dev)
{
    LR1121State *s = LR1121(dev);
    lr1121_load_defaults(s);
}

static Property lr1121_properties[] = {
    DEFINE_PROP_UINT8("spi_id", LR1121State, spi_id, 0),
    DEFINE_PROP_CHR("air-chardev", LR1121State, air_bus.chr),
    DEFINE_PROP_STRING("radio-id", LR1121State, radio_id),
    DEFINE_PROP_STRING("tx-log", LR1121State, tx_log_path),
    DEFINE_PROP_END_OF_LIST(),
};

static void lr1121_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *ssc = SSI_PERIPHERAL_CLASS(klass);

    ssc->realize = lr1121_realize;
    ssc->transfer = lr1121_transfer;
    ssc->set_cs = lr1121_set_cs;
    ssc->cs_polarity = SSI_CS_LOW;

    device_class_set_legacy_reset(dc, lr1121_reset);
    device_class_set_props(dc, lr1121_properties);
}

static void lr1121_instance_init(Object *obj)
{
    LR1121State *s = LR1121(obj);

    qdev_init_gpio_out(DEVICE(s), s->dio, LR1121_DIO_COUNT);
    qdev_init_gpio_out_named(DEVICE(s), &s->busy, "busy", 1);
}

static const TypeInfo lr1121_info = {
    .name          = TYPE_LR1121,
    .parent        = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(LR1121State),
    .instance_init = lr1121_instance_init,
    .class_init    = lr1121_class_init,
};

static void lr1121_register_types(void)
{
    type_register_static(&lr1121_info);
}

type_init(lr1121_register_types)
