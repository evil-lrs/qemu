/*
 * Minimal Semtech SX127x LoRa radio SSI peripheral.
 *
 * This is intentionally a small register stub. It is enough for firmware
 * that probes the radio over SPI, but it does not emulate LoRa/RF behavior.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/ssi/sx127x.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/irq.h"
#include "qemu/timer.h"

#define SX127X_REG_FIFO        0x00
#define SX127X_REG_OP_MODE     0x01
#define SX127X_REG_FRF_MSB     0x06
#define SX127X_REG_FRF_MID     0x07
#define SX127X_REG_FRF_LSB     0x08
#define SX127X_REG_PA_CONFIG   0x09
#define SX127X_REG_OCP         0x0b
#define SX127X_REG_LNA         0x0c
#define SX127X_REG_FIFO_ADDR_PTR 0x0d
#define SX127X_REG_FIFO_TX_BASE_ADDR 0x0e
#define SX127X_REG_FIFO_RX_BASE_ADDR 0x0f
#define SX127X_REG_FIFO_RX_CURRENT_ADDR 0x10
#define SX127X_REG_IRQ_FLAGS   0x12
#define SX127X_REG_RX_NB_BYTES 0x13
#define SX127X_REG_MODEM_CONFIG1 0x1d
#define SX127X_REG_HOP_CHANNEL 0x1c
#define SX127X_REG_MODEM_CONFIG2 0x1e
#define SX127X_REG_PREAMBLE_MSB 0x20
#define SX127X_REG_PREAMBLE_LSB 0x21
#define SX127X_REG_PAYLOAD_LENGTH 0x22
#define SX127X_REG_MODEM_CONFIG3 0x26
#define SX127X_REG_DETECT_OPTIMIZE 0x31
#define SX127X_REG_DETECTION_THRESHOLD 0x37
#define SX127X_REG_SYNC_WORD  0x39
#define SX127X_REG_DIO_MAPPING1 0x40
#define SX127X_REG_VERSION     0x42
#define SX127X_IRQ_VALID_HEADER 0x10
#define SX127X_IRQ_PAYLOAD_CRC_ERROR 0x20
#define SX127X_IRQ_RX_DONE     0x40
#define SX127X_IRQ_TX_DONE     0x08
#define SX127X_HOP_CHANNEL_CRC_ON_PAYLOAD 0x40
#define SX127X_MODEM_CONFIG2_RX_PAYLOAD_CRC_ON 0x04
#define SX127X_TRACE_RAW_SPI   0
#define SX127X_TX_DONE_DELAY_NS 200000

static const char *sx127x_reg_name(uint8_t addr)
{
    switch (addr) {
    case SX127X_REG_FIFO:
        return "RegFifo";
    case SX127X_REG_OP_MODE:
        return "RegOpMode";
    case SX127X_REG_PA_CONFIG:
        return "RegPaConfig";
    case SX127X_REG_OCP:
        return "RegOcp";
    case SX127X_REG_LNA:
        return "RegLna";
    case SX127X_REG_FIFO_ADDR_PTR:
        return "RegFifoAddrPtr";
    case SX127X_REG_FIFO_TX_BASE_ADDR:
        return "RegFifoTxBaseAddr";
    case SX127X_REG_FIFO_RX_BASE_ADDR:
        return "RegFifoRxBaseAddr";
    case SX127X_REG_FIFO_RX_CURRENT_ADDR:
        return "RegFifoRxCurrentAddr";
    case SX127X_REG_IRQ_FLAGS:
        return "RegIrqFlags";
    case SX127X_REG_RX_NB_BYTES:
        return "RegRxNbBytes";
    case SX127X_REG_HOP_CHANNEL:
        return "RegHopChannel";
    case SX127X_REG_PAYLOAD_LENGTH:
        return "RegPayloadLength";
    case SX127X_REG_DIO_MAPPING1:
        return "RegDioMapping1";
    case SX127X_REG_VERSION:
        return "RegVersion";
    default:
        return NULL;
    }
}

static void sx127x_trace_time(SX127xState *s, uint64_t *h, uint64_t *m,
                              uint64_t *sec)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->trace_start_ns == 0) {
        s->trace_start_ns = now;
    }

    uint64_t elapsed = (now - s->trace_start_ns) / 1000000000ULL;
    *h = elapsed / 3600;
    *m = (elapsed / 60) % 60;
    *sec = elapsed % 60;
}

static void sx127x_trace_event(SX127xState *s, const char *fmt, ...)
{
    uint64_t h, m, sec;
    va_list ap;
    g_autofree char *event = NULL;

    sx127x_trace_time(s, &h, &m, &sec);

    va_start(ap, fmt);
    event = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "SX127X[%s]: %02" PRIu64 ":%02" PRIu64 ":%02" PRIu64
                  " - %s\n",
                  s->radio_id ? s->radio_id : "radio", h, m, sec, event);
}

static const char *sx127x_bw_name(uint8_t bw)
{
    switch (bw) {
    case 0x0:
        return "7.8";
    case 0x1:
        return "10.4";
    case 0x2:
        return "15.6";
    case 0x3:
        return "20.8";
    case 0x4:
        return "31.25";
    case 0x5:
        return "41.7";
    case 0x6:
        return "62.5";
    case 0x7:
        return "125";
    case 0x8:
        return "250";
    case 0x9:
        return "500";
    default:
        return "?";
    }
}

static const char *sx127x_mode_name(uint8_t mode)
{
    switch (mode & 0x07) {
    case 0x00:
        return "sleep";
    case 0x01:
        return "standby";
    case 0x02:
        return "fstx";
    case 0x03:
        return "tx";
    case 0x04:
        return "fsrx";
    case 0x05:
        return "rxContinuous";
    case 0x06:
        return "rxSingle";
    case 0x07:
        return "cad";
    default:
        return "unknown";
    }
}

static void sx127x_trace_frequency(SX127xState *s)
{
    uint32_t frf = (s->regs[0x06] << 16) | (s->regs[0x07] << 8) | s->regs[0x08];

    if (frf == s->trace_frf) {
        return;
    }

    s->trace_frf = frf;
    sx127x_trace_event(s, "setRf %.3fMHz raw=0x%06x",
                       (double)frf * 32000000.0 / 524288.0 / 1000000.0,
                       frf);
}

static void sx127x_trace_modulation(SX127xState *s)
{
    uint8_t mc1 = s->regs[SX127X_REG_MODEM_CONFIG1];
    uint8_t mc2 = s->regs[SX127X_REG_MODEM_CONFIG2];
    uint8_t mc3 = s->regs[SX127X_REG_MODEM_CONFIG3];

    if (mc1 == s->trace_modem_config1 &&
        mc2 == s->trace_modem_config2 &&
        mc3 == s->trace_modem_config3) {
        return;
    }

    s->trace_modem_config1 = mc1;
    s->trace_modem_config2 = mc2;
    s->trace_modem_config3 = mc3;

    sx127x_trace_event(s,
                       "setModulationParams SF%u/BW%s/CR4/%u %s CRC%s LDRO%s AGC%s",
                       mc2 >> 4, sx127x_bw_name(mc1 >> 4),
                       ((mc1 >> 1) & 0x07) + 4,
                       (mc1 & 0x01) ? "implicit" : "explicit",
                       (mc2 & SX127X_MODEM_CONFIG2_RX_PAYLOAD_CRC_ON) ? "on" : "off",
                       (mc3 & 0x08) ? "on" : "off",
                       (mc3 & 0x04) ? "on" : "off");
}

static void sx127x_trace_packet_params(SX127xState *s)
{
    uint16_t preamble = (s->regs[SX127X_REG_PREAMBLE_MSB] << 8) |
                        s->regs[SX127X_REG_PREAMBLE_LSB];
    uint8_t payload_len = s->regs[SX127X_REG_PAYLOAD_LENGTH];
    uint8_t sync_word = s->regs[SX127X_REG_SYNC_WORD];

    if (preamble == s->trace_preamble_length &&
        payload_len == s->trace_payload_length &&
        sync_word == s->trace_sync_word) {
        return;
    }

    s->trace_preamble_length = preamble;
    s->trace_payload_length = payload_len;
    s->trace_sync_word = sync_word;

    sx127x_trace_event(s, "setPacketParams preamble=%u payloadLen=%u syncWord=0x%02x",
                       preamble, payload_len, sync_word);
}

static void sx127x_trace_op_mode(SX127xState *s)
{
    uint8_t op_mode = s->regs[SX127X_REG_OP_MODE];

    if (op_mode == s->trace_op_mode) {
        return;
    }

    s->trace_op_mode = op_mode;
    sx127x_trace_event(s, "setMode %s packet=%s raw=0x%02x",
                       sx127x_mode_name(op_mode),
                       (op_mode & 0x80) ? "LoRa" : "FSK/OOK",
                       op_mode);
}

static uint8_t sx127x_effective_op_mode(SX127xState *s, uint8_t value)
{
    uint8_t old = s->regs[SX127X_REG_OP_MODE];

    /*
     * LongRangeMode is latched while the chip is in sleep. Some drivers later
     * write only the low operating-mode bits, so preserve the packet-type bit
     * for non-sleep target modes.
     */
    if ((value & 0x07) != 0 && (value & 0x80) != (old & 0x80)) {
        value = (value & 0x7f) | (old & 0x80);
    }

    return value;
}

static void sx127x_trace_radio_config(SX127xState *s, uint8_t addr)
{
    switch (addr) {
    case SX127X_REG_PA_CONFIG:
        if (s->regs[addr] != s->trace_pa_config) {
            s->trace_pa_config = s->regs[addr];
            sx127x_trace_event(s, "setOutputPower pa=%s raw=0x%02x",
                               (s->regs[addr] & 0x80) ? "PA_BOOST" : "RFO",
                               s->regs[addr]);
        }
        break;
    case SX127X_REG_OCP:
        if (s->regs[addr] != s->trace_ocp) {
            s->trace_ocp = s->regs[addr];
            sx127x_trace_event(s, "setOcp raw=0x%02x",
                               s->regs[addr]);
        }
        break;
    case SX127X_REG_LNA:
        if (s->regs[addr] != s->trace_lna) {
            s->trace_lna = s->regs[addr];
            sx127x_trace_event(s, "setLna raw=0x%02x",
                               s->regs[addr]);
        }
        break;
    case SX127X_REG_DIO_MAPPING1:
        if (s->regs[addr] != s->trace_dio_mapping1) {
            s->trace_dio_mapping1 = s->regs[addr];
            sx127x_trace_event(s, "setDioMapping1 raw=0x%02x",
                               s->regs[addr]);
        }
        break;
    case SX127X_REG_DETECT_OPTIMIZE:
        if (s->regs[addr] != s->trace_detect_optimize) {
            s->trace_detect_optimize = s->regs[addr];
            sx127x_trace_event(s, "setDetectOptimize raw=0x%02x",
                               s->regs[addr]);
        }
        break;
    case SX127X_REG_DETECTION_THRESHOLD:
        if (s->regs[addr] != s->trace_detection_threshold) {
            s->trace_detection_threshold = s->regs[addr];
            sx127x_trace_event(s, "setDetectionThreshold raw=0x%02x",
                               s->regs[addr]);
        }
        break;
    default:
        break;
    }
}

static void sx127x_trace_config_write(SX127xState *s, uint8_t addr)
{
    switch (addr) {
    case SX127X_REG_FRF_MSB:
    case SX127X_REG_FRF_MID:
    case SX127X_REG_FRF_LSB:
        sx127x_trace_frequency(s);
        break;
    case SX127X_REG_MODEM_CONFIG1:
    case SX127X_REG_MODEM_CONFIG2:
    case SX127X_REG_MODEM_CONFIG3:
        sx127x_trace_modulation(s);
        break;
    case SX127X_REG_PREAMBLE_MSB:
    case SX127X_REG_PREAMBLE_LSB:
    case SX127X_REG_PAYLOAD_LENGTH:
    case SX127X_REG_SYNC_WORD:
        sx127x_trace_packet_params(s);
        break;
    case SX127X_REG_OP_MODE:
        sx127x_trace_op_mode(s);
        break;
    default:
        sx127x_trace_radio_config(s, addr);
        break;
    }
}

static void sx127x_load_defaults(SX127xState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->fifo, 0, sizeof(s->fifo));

    s->regs[0x01] = 0x01; /* RegOpMode */
    s->regs[0x06] = 0x6c; /* RegFrfMsb */
    s->regs[0x07] = 0x80; /* RegFrfMid */
    s->regs[0x08] = 0x00; /* RegFrfLsb */
    s->regs[0x0c] = 0x23; /* RegLna */
    s->regs[0x0e] = 0x80; /* RegFifoTxBaseAddr */
    s->regs[0x0f] = 0x00; /* RegFifoRxBaseAddr */
    s->regs[0x10] = 0x00; /* RegFifoRxCurrentAddr */
    s->regs[SX127X_REG_IRQ_FLAGS] = 0x00;
    s->regs[0x13] = 0x00; /* RegRxNbBytes */
    s->regs[0x19] = 0x00; /* RegPktSnrValue */
    s->regs[0x1a] = 0x00; /* RegPktRssiValue */
    s->regs[0x1d] = 0x72; /* RegModemConfig1 */
    s->regs[SX127X_REG_MODEM_CONFIG2] = 0x74; /* RegModemConfig2 */
    s->regs[0x20] = 0x00; /* RegPreambleMsb */
    s->regs[0x21] = 0x08; /* RegPreambleLsb */
    s->regs[0x22] = 0x00; /* RegPayloadLength */
    s->regs[0x26] = 0x04; /* RegModemConfig3 */
    s->regs[0x39] = 0x12; /* RegSyncWord */
    s->regs[0x40] = 0x00; /* RegDioMapping1 */
    s->regs[SX127X_REG_VERSION] = 0x12;
    s->regs[0x4d] = 0x84; /* RegPaDac */

    s->have_addr = false;
    s->is_write = false;
    s->data_count = 0;
    s->addr = 0;
    s->fifo_pos = 0;
    s->trace_start_ns = 0;
    s->trace_frf = UINT32_MAX;
    s->trace_op_mode = UINT8_MAX;
    s->trace_modem_config1 = UINT8_MAX;
    s->trace_modem_config2 = UINT8_MAX;
    s->trace_modem_config3 = UINT8_MAX;
    s->trace_payload_length = UINT8_MAX;
    s->trace_preamble_length = UINT16_MAX;
    s->trace_sync_word = UINT8_MAX;
    s->trace_pa_config = UINT8_MAX;
    s->trace_ocp = UINT8_MAX;
    s->trace_lna = UINT8_MAX;
    s->trace_dio_mapping1 = UINT8_MAX;
    s->trace_detect_optimize = UINT8_MAX;
    s->trace_detection_threshold = UINT8_MAX;
    memset(s->dio_level, 0, sizeof(s->dio_level));
}

static int sx127x_set_cs(SSIPeripheral *ss, bool select)
{
    SX127xState *s = SX127X(ss);
    bool is_selected = (select == (ss->spc->cs_polarity == SSI_CS_HIGH));

    if (SX127X_TRACE_RAW_SPI) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SX127X[SPI%d:CS%d]: set_cs level=%d (selected=%d, have_addr=%d)\n",
                      s->spi_id, s->parent_obj.cs_index, select, is_selected,
                      s->have_addr);
    }

    if (is_selected && !s->selected && (!s->have_addr || s->data_count != 0)) {
        s->have_addr = false;
        s->data_count = 0;
    }
    s->selected = is_selected;

    return 0;
}

static void sx127x_update_irq(SX127xState *s)
{
    /*
     * Simple hack: if RegIrqFlags has any bits set, pull DIO0 high.
     * In SX127x, DIO0 mapping 00 is RxDone or TxDone.
     */
    bool irq = s->regs[SX127X_REG_IRQ_FLAGS] != 0;

    if (s->dio_level[0] != irq) {
        if (SX127X_TRACE_RAW_SPI) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "SX127X[SPI%d:CS%d]: DIO0 %s irq=0x%02x\n",
                          s->spi_id, s->parent_obj.cs_index,
                          irq ? "high" : "low", s->regs[SX127X_REG_IRQ_FLAGS]);
        }
        s->dio_level[0] = irq;
    }
    qemu_set_irq(s->dio[0], irq);
}

static void sx127x_tx_done_cb(void *opaque)
{
    SX127xState *s = SX127X(opaque);

    s->regs[SX127X_REG_IRQ_FLAGS] |= SX127X_IRQ_TX_DONE;
    sx127x_update_irq(s);
}

static void sx127x_rx_cb(void *opaque, const SemtechRadioFrame *f)
{
    SX127xState *s = opaque;
    uint8_t mode = s->regs[SX127X_REG_OP_MODE] & 0x07;
    bool rx_enabled = (mode == 0x05 || mode == 0x06);

    if (!rx_enabled) {
        return;
    }

    uint8_t base = s->regs[SX127X_REG_FIFO_RX_BASE_ADDR];
    size_t len = MIN(f->payload_len, sizeof(s->fifo));

    for (size_t i = 0; i < len; i++) {
        s->fifo[(uint8_t)(base + i)] = f->payload[i];
    }

    s->regs[SX127X_REG_FIFO_RX_CURRENT_ADDR] = base;
    s->regs[SX127X_REG_RX_NB_BYTES] = len;
    s->regs[0x19] = (uint8_t)(f->snr_db * 4);  /* RegPktSnrValue */
    s->regs[0x1a] = (uint8_t)(f->rssi_dbm + 137); /* RegPktRssiValue (offset for LoRa) */
    if (s->regs[SX127X_REG_MODEM_CONFIG2] & SX127X_MODEM_CONFIG2_RX_PAYLOAD_CRC_ON) {
        s->regs[SX127X_REG_HOP_CHANNEL] |= SX127X_HOP_CHANNEL_CRC_ON_PAYLOAD;
    } else {
        s->regs[SX127X_REG_HOP_CHANNEL] &= ~SX127X_HOP_CHANNEL_CRC_ON_PAYLOAD;
    }
    s->regs[SX127X_REG_IRQ_FLAGS] &= ~SX127X_IRQ_PAYLOAD_CRC_ERROR;
    s->regs[SX127X_REG_IRQ_FLAGS] |= SX127X_IRQ_RX_DONE | SX127X_IRQ_VALID_HEADER;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "SX127X[%s]: RX injected len=%zu rssi=%d snr=%d\n",
                  s->radio_id, len, f->rssi_dbm, f->snr_db);

    sx127x_update_irq(s);
}

static void sx127x_send_hello(SX127xState *s)
{
    SemtechRadioFrame f = {
        .chip = "sx127x",
        .radio_id = s->radio_id,
    };
    GString *out = g_string_new("");
    semtech_frame_to_hello_json(&f, out);
    if (semtech_air_bus_send_json(&s->air_bus, out->str)) {
        qemu_log_mask(LOG_GUEST_ERROR, "SX127X[%s]: sent hello to air-bus\n", s->radio_id);
    }
    g_string_free(out, true);
}

static void sx127x_send_state(SX127xState *s)
{
    uint32_t frf = (s->regs[0x06] << 16) | (s->regs[0x07] << 8) | s->regs[0x08];
    uint64_t freq_hz = (uint64_t)frf * 32000000ULL / 524288ULL;
    bool lora = (s->regs[SX127X_REG_OP_MODE] & 0x80) != 0;
    uint8_t mode = s->regs[SX127X_REG_OP_MODE] & 0x07;
    bool rx_enabled = (mode == 0x05 || mode == 0x06); /* RXCONTINUOUS or RXSINGLE */

    SemtechRadioFrame f = {
        .radio_id = s->radio_id,
        .freq_hz = freq_hz,
        .packet_type = lora ? "lora" : "fsk",
        .sync_word_len = 1,
    };
    f.sync_word[0] = s->regs[0x39]; /* RegSyncWord */

    GString *out = g_string_new("");
    semtech_frame_to_state_json(&f, rx_enabled, out);
    if (semtech_air_bus_send_json(&s->air_bus, out->str)) {
        qemu_log_mask(LOG_GUEST_ERROR, "SX127X[%s]: sent state to air-bus (rx=%d freq=%" PRIu64 ")\n",
                      s->radio_id, rx_enabled, freq_hz);
    }
    g_string_free(out, true);
}

static void sx127x_send_tx(SX127xState *s)
{
    uint8_t base = s->regs[SX127X_REG_FIFO_TX_BASE_ADDR];
    uint8_t len = s->regs[SX127X_REG_PAYLOAD_LENGTH];
    uint32_t frf = (s->regs[0x06] << 16) | (s->regs[0x07] << 8) | s->regs[0x08];
    uint64_t freq_hz = (uint64_t)frf * 32000000ULL / 524288ULL;
    bool lora = (s->regs[SX127X_REG_OP_MODE] & 0x80) != 0;

    SemtechRadioFrame f = {
        .chip = "sx127x",
        .radio_id = s->radio_id,
        .freq_hz = freq_hz,
        .packet_type = lora ? "lora" : "fsk",
        .payload_len = len,
        .sync_word_len = 1,
    };
    f.sync_word[0] = s->regs[0x39];

    for (int i = 0; i < len; i++) {
        f.payload[i] = s->fifo[(uint8_t)(base + i)];
    }

    GString *js = g_string_new("");
    semtech_frame_to_tx_json(&f, js);

    if (semtech_air_bus_send_json(&s->air_bus, js->str)) {
        qemu_log_mask(LOG_GUEST_ERROR, "SX127X[%s]: TX sent to air-bus (len=%u freq=%" PRIu64 ")\n",
                      s->radio_id, len, freq_hz);
    } else {
        semtech_frame_log_tx(&f, s->tx_log_path);
        qemu_log_mask(LOG_GUEST_ERROR, "SX127X[%s]: TX logged to file (len=%u freq=%" PRIu64 ")\n",
                      s->radio_id, len, freq_hz);
    }

    g_string_free(js, true);
}

static uint8_t sx127x_read_reg(SX127xState *s, uint8_t addr)
{
    uint8_t value;

    if (addr == SX127X_REG_FIFO) {
        value = s->fifo[s->fifo_pos++];
        s->regs[SX127X_REG_FIFO_ADDR_PTR] = s->fifo_pos;
    } else {
        value = s->regs[addr & 0x7f];
    }

    uint8_t radio_id = s->parent_obj.cs_index;

    if (!SX127X_TRACE_RAW_SPI) {
        return value;
    }
    if (sx127x_reg_name(addr)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SX127X[SPI%d:CS%d]: %s read -> 0x%02x\n",
                      s->spi_id, radio_id, sx127x_reg_name(addr), value);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SX127X[SPI%d:CS%d]: addr=0x%02x read -> 0x%02x\n",
                      s->spi_id, radio_id, addr, value);
    }
    return value;
}

static void sx127x_write_reg(SX127xState *s, uint8_t addr, uint8_t value)
{
    uint8_t old_mode = s->regs[SX127X_REG_OP_MODE];

    if (addr == SX127X_REG_FIFO) {
        s->fifo[s->fifo_pos++] = value;
        s->regs[SX127X_REG_FIFO_ADDR_PTR] = s->fifo_pos;
    } else if (addr == SX127X_REG_IRQ_FLAGS) {
        s->regs[SX127X_REG_IRQ_FLAGS] &= ~value;
    } else if (addr == SX127X_REG_OP_MODE) {
        value = sx127x_effective_op_mode(s, value);
        s->regs[addr & 0x7f] = value;
        if ((value & 0x07) == 0x03) { /* TX mode */
            s->regs[SX127X_REG_IRQ_FLAGS] &= ~SX127X_IRQ_TX_DONE;
            sx127x_update_irq(s);
            sx127x_send_tx(s);
            timer_mod(&s->tx_done_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      SX127X_TX_DONE_DELAY_NS);
        }
        if (value != old_mode) {
            sx127x_send_state(s);
        }
        sx127x_trace_config_write(s, addr);
    } else {
        s->regs[addr & 0x7f] = value;
        if (addr == SX127X_REG_FIFO_ADDR_PTR) {
            s->fifo_pos = value;
        }
        /* Send state on config changes */
        if (addr == 0x06 || addr == 0x07 || addr == 0x08 || /* Frf */
            addr == 0x1d || addr == 0x1e || addr == 0x26 || /* ModemConfig */
            addr == 0x39) { /* SyncWord */
            sx127x_send_state(s);
        }
        sx127x_trace_config_write(s, addr);
    }

    sx127x_update_irq(s);

    uint8_t radio_id = s->parent_obj.cs_index;

    if (!SX127X_TRACE_RAW_SPI) {
        return;
    }
    if (sx127x_reg_name(addr)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SX127X[SPI%d:CS%d]: %s write 0x%02x\n",
                      s->spi_id, radio_id, sx127x_reg_name(addr), value);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SX127X[SPI%d:CS%d]: addr=0x%02x write 0x%02x\n",
                      s->spi_id, radio_id, addr, value);
    }
}

static uint32_t sx127x_transfer(SSIPeripheral *ss, uint32_t tx)
{
    SX127xState *s = SX127X(ss);
    uint8_t byte = tx;
    uint8_t ret = 0;
    uint8_t radio_id = s->parent_obj.cs_index;

    if (!s->selected) {
        return 0;
    }

    if (SX127X_TRACE_RAW_SPI) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SX127X[SPI%d:CS%d]: transfer byte=0x%02x (have_addr=%d, addr=0x%02x, is_write=%d)\n",
                      s->spi_id, radio_id, byte, s->have_addr, s->addr,
                      s->is_write);
    }

    if (!s->have_addr) {
        s->is_write = (byte & 0x80) != 0;
        s->addr = byte & 0x7f;
        s->have_addr = true;
        s->data_count = 0;
        if (SX127X_TRACE_RAW_SPI) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "SX127X[SPI%d:CS%d]: command addr=0x%02x %s\n",
                          s->spi_id, radio_id, s->addr,
                          s->is_write ? "write" : "read");
        }
        return 0;
    }

    if (s->is_write) {
        sx127x_write_reg(s, s->addr, byte);
    } else {
        ret = sx127x_read_reg(s, s->addr);
    }

    /*
     * Increment address only AFTER processing a data byte.
     * This allows the first data byte to hit the address specified in the command byte.
     */
    if (s->addr != SX127X_REG_FIFO) {
        s->addr = (s->addr + 1) & 0x7f;
    }
    s->data_count++;
    return ret;
}

static void sx127x_realize(SSIPeripheral *ss, Error **errp)
{
    SX127xState *s = SX127X(ss);

    if (!s->radio_id) {
        s->radio_id = g_strdup_printf("sx127x-%d", s->parent_obj.cs_index);
    }

    semtech_air_bus_init(&s->air_bus, sx127x_rx_cb, s);
    semtech_air_bus_start(&s->air_bus);
    sx127x_send_hello(s);
    sx127x_send_state(s);

    sx127x_load_defaults(s);
}

static void sx127x_reset(DeviceState *dev)
{
    SX127xState *s = SX127X(dev);

    timer_del(&s->tx_done_timer);
    sx127x_load_defaults(s);
}

static Property sx127x_properties[] = {
    DEFINE_PROP_UINT8("spi_id", SX127xState, spi_id, 0),
    DEFINE_PROP_CHR("air-chardev", SX127xState, air_bus.chr),
    DEFINE_PROP_STRING("radio-id", SX127xState, radio_id),
    DEFINE_PROP_STRING("tx-log", SX127xState, tx_log_path),
    DEFINE_PROP_END_OF_LIST(),
};

static void sx127x_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *ssc = SSI_PERIPHERAL_CLASS(klass);

    ssc->realize = sx127x_realize;
    ssc->transfer = sx127x_transfer;
    ssc->set_cs = sx127x_set_cs;
    ssc->cs_polarity = SSI_CS_LOW;
    device_class_set_legacy_reset(dc, sx127x_reset);
    device_class_set_props(dc, sx127x_properties);
}

static void sx127x_instance_init(Object *obj)
{
    SX127xState *s = SX127X(obj);

    timer_init_ns(&s->tx_done_timer, QEMU_CLOCK_VIRTUAL,
                  sx127x_tx_done_cb, s);
    qdev_init_gpio_out_named(DEVICE(s), s->dio, SX127X_DIO_GPIO, 6);
}

static const TypeInfo sx127x_info = {
    .name = TYPE_SX127X,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(SX127xState),
    .instance_init = sx127x_instance_init,
    .class_init = sx127x_class_init,
};

static void sx127x_register_types(void)
{
    type_register_static(&sx127x_info);
}

type_init(sx127x_register_types)

void sx127x_linker_anchor(void)
{
}
