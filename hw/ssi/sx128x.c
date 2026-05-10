/*
 * Minimal Semtech SX128x (2.4 GHz) SSI peripheral.
 *
 * Stub model: enough to let firmware probe the chip, configure it, and
 * issue a TX. RX is not modeled. Everything is acknowledged with a status
 * byte; TX immediately raises TxDone.
 *
 * Protocol reference: Semtech SX1280/SX1281 datasheet rev 3.2 (June 2020),
 * section 11 ("SPI Interface") and section 13 ("Operational Modes").
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/ssi/sx128x.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"

/* Opcodes (table 11-1). Only the ones we treat specially are named; the
 * rest are accepted with a parameter count taken from sx128x_param_count(). */
#define SX128X_OP_GET_STATUS              0xC0
#define SX128X_OP_WRITE_REGISTER          0x18
#define SX128X_OP_READ_REGISTER           0x19
#define SX128X_OP_WRITE_BUFFER            0x1A
#define SX128X_OP_READ_BUFFER             0x1B
#define SX128X_OP_SET_SLEEP               0x84
#define SX128X_OP_SET_STANDBY             0x80
#define SX128X_OP_SET_FS                  0xC1
#define SX128X_OP_SET_TX                  0x83
#define SX128X_OP_SET_RX                  0x82
#define SX128X_OP_SET_RF_FREQUENCY        0x86
#define SX128X_OP_SET_PACKET_TYPE         0x8A
#define SX128X_OP_SET_PACKET_PARAMS       0x8C
#define SX128X_OP_SET_MODULATION_PARAMS   0x8B
#define SX128X_OP_SET_BUFFER_BASE_ADDRESS 0x8F
#define SX128X_OP_GET_IRQ_STATUS          0x15
#define SX128X_OP_CLEAR_IRQ_STATUS        0x97
#define SX128X_OP_GET_PACKET_STATUS       0x1D
#define SX128X_OP_GET_PACKET_TYPE         0x03
#define SX128X_OP_SET_DIO_IRQ_PARAMS      0x8D
#define SX128X_OP_SET_TX_PARAMS           0x8E
#define SX128X_OP_SET_AUTO_FS             0x9E
#define SX128X_OP_SET_LONG_PREAMBLE       0x9B
#define SX128X_OP_SET_REGULATOR_MODE      0x96
#define SX128X_OP_GET_RX_BUFFER_STATUS    0x17

/* IRQ bits (table 11-79) */
#define SX128X_IRQ_TX_DONE        0x0001
#define SX128X_IRQ_RX_DONE        0x0002
#define SX128X_IRQ_HEADER_VALID   0x0010
#define SX128X_IRQ_HEADER_ERROR   0x0020
#define SX128X_IRQ_CRC_ERROR      0x0040
#define SX128X_IRQ_RX_TX_TIMEOUT  0x4000

/* Encoded status byte: mode=STDBY_RC (0x2 << 5) | cmd_status=OK (0x1 << 2). */
#define SX128X_DEFAULT_STATUS  ((0x2 << 5) | (0x1 << 2))

static const char *sx128x_opcode_name(uint8_t op)
{
    switch (op) {
    case SX128X_OP_GET_STATUS:              return "GetStatus";
    case SX128X_OP_WRITE_REGISTER:          return "WriteRegister";
    case SX128X_OP_READ_REGISTER:           return "ReadRegister";
    case SX128X_OP_WRITE_BUFFER:            return "WriteBuffer";
    case SX128X_OP_READ_BUFFER:             return "ReadBuffer";
    case SX128X_OP_SET_SLEEP:               return "SetSleep";
    case SX128X_OP_SET_STANDBY:             return "SetStandby";
    case SX128X_OP_SET_FS:                  return "SetFs";
    case SX128X_OP_SET_TX:                  return "SetTx";
    case SX128X_OP_SET_RX:                  return "SetRx";
    case SX128X_OP_SET_RF_FREQUENCY:        return "SetRfFrequency";
    case SX128X_OP_SET_PACKET_TYPE:         return "SetPacketType";
    case SX128X_OP_SET_PACKET_PARAMS:       return "SetPacketParams";
    case SX128X_OP_SET_MODULATION_PARAMS:   return "SetModulationParams";
    case SX128X_OP_SET_BUFFER_BASE_ADDRESS: return "SetBufferBaseAddress";
    case SX128X_OP_GET_IRQ_STATUS:          return "GetIrqStatus";
    case SX128X_OP_CLEAR_IRQ_STATUS:        return "ClearIrqStatus";
    case SX128X_OP_GET_PACKET_STATUS:       return "GetPacketStatus";
    case SX128X_OP_GET_PACKET_TYPE:         return "GetPacketType";
    case SX128X_OP_SET_DIO_IRQ_PARAMS:      return "SetDioIrqParams";
    case SX128X_OP_SET_TX_PARAMS:           return "SetTxParams";
    case SX128X_OP_SET_AUTO_FS:             return "SetAutoFs";
    case SX128X_OP_SET_LONG_PREAMBLE:       return "SetLongPreamble";
    case SX128X_OP_SET_REGULATOR_MODE:      return "SetRegulatorMode";
    case SX128X_OP_GET_RX_BUFFER_STATUS:    return "GetRxBufferStatus";
    default:                                return NULL;
    }
}

/*
 * Number of parameter bytes the host clocks AFTER the opcode, BEFORE we
 * branch into a special data phase (or finish for plain "set" opcodes).
 *
 * For getter opcodes the host first clocks 1 NOP byte (to give the chip
 * time to load the answer) and then reads N bytes; we account for that NOP
 * in param_needed and produce the actual response data after.
 */
static uint8_t sx128x_param_count(uint8_t op)
{
    switch (op) {
    case SX128X_OP_GET_STATUS:              return 0; /* status returned with the opcode byte itself */
    case SX128X_OP_SET_STANDBY:             return 1;
    case SX128X_OP_SET_PACKET_TYPE:         return 1;
    case SX128X_OP_SET_REGULATOR_MODE:      return 1;
    case SX128X_OP_SET_AUTO_FS:             return 1;
    case SX128X_OP_SET_LONG_PREAMBLE:       return 1;
    case SX128X_OP_SET_SLEEP:               return 1;
    case SX128X_OP_SET_BUFFER_BASE_ADDRESS: return 2;
    case SX128X_OP_SET_TX_PARAMS:           return 2;
    case SX128X_OP_CLEAR_IRQ_STATUS:        return 2;
    case SX128X_OP_SET_MODULATION_PARAMS:   return 3;
    case SX128X_OP_SET_TX:                  return 3;
    case SX128X_OP_SET_RX:                  return 3;
    case SX128X_OP_GET_IRQ_STATUS:          return 3; /* 1 NOP + 2 status bytes */
    case SX128X_OP_GET_RX_BUFFER_STATUS:    return 3; /* 1 NOP + 2 bytes */
    case SX128X_OP_GET_PACKET_TYPE:         return 2; /* 1 NOP + 1 byte */
    case SX128X_OP_SET_FS:                  return 0;
    case SX128X_OP_GET_PACKET_STATUS:       return 6; /* 1 NOP + 5 bytes */
    case SX128X_OP_SET_RF_FREQUENCY:        return 3; /* SX128x freq is 3 bytes (24-bit) */
    case SX128X_OP_SET_PACKET_PARAMS:       return 7;
    case SX128X_OP_SET_DIO_IRQ_PARAMS:      return 8;
    /* For WriteRegister/ReadRegister/WriteBuffer/ReadBuffer the address
     * phase sets up io_remaining; param_needed is just the address part. */
    case SX128X_OP_WRITE_REGISTER:          return 2; /* 16-bit addr, then variable data */
    case SX128X_OP_READ_REGISTER:           return 2;
    case SX128X_OP_WRITE_BUFFER:            return 1; /* 8-bit offset, then data */
    case SX128X_OP_READ_BUFFER:             return 1;
    default:                                return 0;
    }
}

static void sx128x_load_defaults(SX128xState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->buf, 0, sizeof(s->buf));
    memset(s->packet_params, 0, sizeof(s->packet_params));
    memset(s->modulation_params, 0, sizeof(s->modulation_params));

    s->status = SX128X_DEFAULT_STATUS;
    s->phase = SX128X_STATE_IDLE;
    s->opcode = 0;
    s->param_pos = 0;
    s->param_needed = 0;
    s->reg_addr = 0;
    s->buf_addr = 0;
    s->io_remaining = 0;
    s->nop_consumed = false;

    s->buf_tx_base = 0x00;
    s->buf_rx_base = 0x00;
    s->irq_status = 0;
    s->irq_mask = 0;
    s->irq_dio1 = 0;
    s->irq_dio2 = 0;
    s->irq_dio3 = 0;

    s->packet_type = 0;
    s->rf_freq_raw = 0;
    s->tx_payload_len = 0;

    /*
     * RadioLib's findChip() reads the version string and memcmps the
     * first 6 bytes against "SX1280". Newer RadioLib reads from 0x01F0
     * (RADIOLIB_SX128X_REG_FIRMWARE_VERSION_MSB in current releases);
     * older code expected 0x0153. Plant the literal at both addresses
     * so either driver version recognizes the chip.
     */
    static const char version_str[6] = { 'S', 'X', '1', '2', '8', '0' };
    memcpy(&s->regs[0x0153], version_str, sizeof(version_str));
    memcpy(&s->regs[0x01F0], version_str, sizeof(version_str));

    memset(s->dio_level, 0, sizeof(s->dio_level));
}

static int sx128x_set_cs(SSIPeripheral *ss, bool select)
{
    SX128xState *s = SX128X(ss);
    bool is_selected = (select == (ss->spc->cs_polarity == SSI_CS_HIGH));

    qemu_log_mask(LOG_GUEST_ERROR,
                  "SX128X[SPI%d:CS%d]: set_cs level=%d (selected=%d phase=%d)\n",
                  s->spi_id, s->parent_obj.cs_index, select, is_selected,
                  s->phase);

    if (is_selected && !s->selected) {
        /* Start of a new transaction. */
        s->phase = SX128X_STATE_IDLE;
        s->param_pos = 0;
        s->param_needed = 0;
        s->io_remaining = 0;
        s->nop_consumed = false;
    }
    s->selected = is_selected;
    return 0;
}

static void sx128x_update_irq(SX128xState *s)
{
    /* Map IRQ bits to DIO pins per the configured masks. */
    bool levels[SX128X_DIO_COUNT] = {
        (s->irq_status & s->irq_dio1) != 0,
        (s->irq_status & s->irq_dio2) != 0,
        (s->irq_status & s->irq_dio3) != 0,
    };

    for (int i = 0; i < SX128X_DIO_COUNT; i++) {
        if (s->dio_level[i] != levels[i]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "SX128X[SPI%d:CS%d]: DIO%d %s irq_status=0x%04x\n",
                          s->spi_id, s->parent_obj.cs_index, i + 1,
                          levels[i] ? "high" : "low", s->irq_status);
            s->dio_level[i] = levels[i];
        }
        qemu_set_irq(s->dio[i], levels[i]);
    }
}

static void sx128x_log_tx_payload(SX128xState *s)
{
    uint8_t base = s->buf_tx_base;
    uint8_t len = s->tx_payload_len;
    char hex[3 * 256 + 1];
    size_t off = 0;

    for (int i = 0; i < len && off < sizeof(hex); i++) {
        off += snprintf(hex + off, sizeof(hex) - off, "%s%02x",
                        i == 0 ? "" : " ", s->buf[(uint8_t)(base + i)]);
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "SX128X[SPI%d:CS%d]: TX len=%u freq_raw=0x%06x "
                  "pkt_type=0x%02x payload=[%s]\n",
                  s->spi_id, s->parent_obj.cs_index, len,
                  s->rf_freq_raw, s->packet_type, hex);
}

/*
 * Apply an opcode whose parameter bytes have been fully clocked in. For
 * "getter" opcodes that returned bytes during the param phase, this is
 * still called for diagnostic logging (the bytes have already been
 * produced via sx128x_param_byte_for_phase).
 */
static void sx128x_complete_opcode(SX128xState *s)
{
    const char *name = sx128x_opcode_name(s->opcode);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "SX128X[SPI%d:CS%d]: opcode 0x%02x (%s) complete (params=%u)\n",
                  s->spi_id, s->parent_obj.cs_index, s->opcode,
                  name ? name : "?", s->param_pos);

    switch (s->opcode) {
    case SX128X_OP_SET_RF_FREQUENCY:
        s->rf_freq_raw = ((uint32_t)s->param_buf[0] << 16) |
                         ((uint32_t)s->param_buf[1] << 8)  |
                          (uint32_t)s->param_buf[2];
        break;
    case SX128X_OP_SET_PACKET_TYPE:
        s->packet_type = s->param_buf[0];
        break;
    case SX128X_OP_SET_PACKET_PARAMS:
        memcpy(s->packet_params, s->param_buf, 7);
        /* PacketParams[2] is PayloadLength for LoRa/Ranging. (For FLRC it
         * is encoded differently, but using it as TX length is good enough
         * for the stub.) */
        s->tx_payload_len = s->param_buf[2];
        break;
    case SX128X_OP_SET_MODULATION_PARAMS:
        memcpy(s->modulation_params, s->param_buf, 3);
        break;
    case SX128X_OP_SET_BUFFER_BASE_ADDRESS:
        s->buf_tx_base = s->param_buf[0];
        s->buf_rx_base = s->param_buf[1];
        break;
    case SX128X_OP_SET_DIO_IRQ_PARAMS:
        s->irq_mask = ((uint16_t)s->param_buf[0] << 8) | s->param_buf[1];
        s->irq_dio1 = ((uint16_t)s->param_buf[2] << 8) | s->param_buf[3];
        s->irq_dio2 = ((uint16_t)s->param_buf[4] << 8) | s->param_buf[5];
        s->irq_dio3 = ((uint16_t)s->param_buf[6] << 8) | s->param_buf[7];
        break;
    case SX128X_OP_CLEAR_IRQ_STATUS: {
        uint16_t mask = ((uint16_t)s->param_buf[0] << 8) | s->param_buf[1];
        s->irq_status &= ~mask;
        break;
    }
    case SX128X_OP_SET_TX:
        sx128x_log_tx_payload(s);
        s->irq_status |= SX128X_IRQ_TX_DONE;
        break;
    case SX128X_OP_SET_RX:
        /* No RX modeling; just log. */
        break;
    case SX128X_OP_WRITE_REGISTER:
        s->reg_addr = ((uint16_t)s->param_buf[0] << 8) | s->param_buf[1];
        /* io_remaining set lazily as data arrives — until CS deasserts. */
        break;
    case SX128X_OP_READ_REGISTER:
        s->reg_addr = ((uint16_t)s->param_buf[0] << 8) | s->param_buf[1];
        break;
    case SX128X_OP_WRITE_BUFFER:
        s->buf_addr = s->param_buf[0];
        break;
    case SX128X_OP_READ_BUFFER:
        s->buf_addr = s->param_buf[0];
        break;
    default:
        break;
    }

    sx128x_update_irq(s);
}

/*
 * Produce the byte to clock back during the parameter phase of getter
 * opcodes. Index `i` is 0-based from the opcode byte.
 */
static uint8_t sx128x_param_byte_for_phase(SX128xState *s, uint8_t i)
{
    switch (s->opcode) {
    case SX128X_OP_GET_IRQ_STATUS:
        if (i == 0) return 0; /* NOP */
        if (i == 1) return s->irq_status >> 8;
        if (i == 2) return s->irq_status & 0xff;
        return 0;
    case SX128X_OP_GET_RX_BUFFER_STATUS:
        if (i == 0) return 0;
        if (i == 1) return 0; /* rxPayloadLength */
        if (i == 2) return s->buf_rx_base;
        return 0;
    case SX128X_OP_GET_PACKET_STATUS:
        return 0; /* RSSI/SNR/errors — all zero for stub */
    case SX128X_OP_GET_PACKET_TYPE:
        if (i == 0) return 0;            /* NOP */
        if (i == 1) return s->packet_type;
        return 0;
    default:
        return 0;
    }
}

static uint32_t sx128x_transfer(SSIPeripheral *ss, uint32_t tx)
{
    SX128xState *s = SX128X(ss);
    uint8_t in = tx;
    uint8_t out = s->status;
    const char *name;

    if (!s->selected) {
        return 0;
    }

    switch (s->phase) {
    case SX128X_STATE_IDLE:
        /* Opcode byte. */
        s->opcode = in;
        s->param_pos = 0;
        s->param_needed = sx128x_param_count(in);
        s->nop_consumed = false;
        name = sx128x_opcode_name(in);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SX128X[SPI%d:CS%d]: opcode 0x%02x (%s) need=%u params\n",
                      s->spi_id, s->parent_obj.cs_index, in,
                      name ? name : "?", s->param_needed);

        if (in == SX128X_OP_GET_STATUS) {
            /* Status returned with the opcode byte; further bytes in the
             * same CS assertion are NOPs that should also yield status. */
            s->phase = SX128X_STATE_DRAIN;
            return s->status;
        }

        if (s->param_needed == 0) {
            sx128x_complete_opcode(s);
            /* The opcode is fully handled. Subsequent bytes (until CS
             * deassert) are NOPs that should clock back status, NOT be
             * interpreted as new opcodes. */
            s->phase = SX128X_STATE_DRAIN;
            return s->status;
        }

        s->phase = SX128X_STATE_PARAMS;
        return s->status;

    case SX128X_STATE_PARAMS: {
        /* Reading: produce the response byte FIRST, then consume the host
         * byte as the (ignored) param. */
        bool is_getter = (s->opcode == SX128X_OP_GET_IRQ_STATUS ||
                          s->opcode == SX128X_OP_GET_RX_BUFFER_STATUS ||
                          s->opcode == SX128X_OP_GET_PACKET_STATUS ||
                          s->opcode == SX128X_OP_GET_PACKET_TYPE);
        if (is_getter) {
            out = sx128x_param_byte_for_phase(s, s->param_pos);
        }

        if (s->param_pos < sizeof(s->param_buf)) {
            s->param_buf[s->param_pos] = in;
        }
        s->param_pos++;

        if (s->param_pos >= s->param_needed) {
            sx128x_complete_opcode(s);
            /* Branch into appropriate data phase for chained ops. */
            switch (s->opcode) {
            case SX128X_OP_WRITE_REGISTER:
                s->phase = SX128X_STATE_REG_DATA;
                s->io_remaining = 0xffff; /* "until CS deasserts" */
                break;
            case SX128X_OP_READ_REGISTER:
                s->phase = SX128X_STATE_REG_READ;
                s->nop_consumed = false;
                break;
            case SX128X_OP_WRITE_BUFFER:
                s->phase = SX128X_STATE_BUF_DATA;
                s->io_remaining = 0xffff;
                break;
            case SX128X_OP_READ_BUFFER:
                s->phase = SX128X_STATE_BUF_READ;
                s->nop_consumed = false;
                break;
            default:
                /* Plain "set" opcode fully consumed; any further bytes
                 * until CS deassert are NOPs returning status. */
                s->phase = SX128X_STATE_DRAIN;
                break;
            }
        }
        return out;
    }

    case SX128X_STATE_DRAIN:
        return s->status;

    case SX128X_STATE_REG_DATA:
        if (s->reg_addr < SX128X_REG_BYTES) {
            s->regs[s->reg_addr] = in;
        }
        s->reg_addr++;
        return s->status;

    case SX128X_STATE_REG_READ:
        if (!s->nop_consumed) {
            /* First byte after the address is a NOP from the host; we
             * return status. */
            s->nop_consumed = true;
            return s->status;
        }
        out = (s->reg_addr < SX128X_REG_BYTES) ? s->regs[s->reg_addr] : 0;
        s->reg_addr++;
        return out;

    case SX128X_STATE_BUF_DATA:
        s->buf[s->buf_addr++] = in;
        return s->status;

    case SX128X_STATE_BUF_READ:
        if (!s->nop_consumed) {
            s->nop_consumed = true;
            return s->status;
        }
        out = s->buf[s->buf_addr++];
        return out;
    }

    return s->status;
}

static void sx128x_realize(SSIPeripheral *ss, Error **errp)
{
    SX128xState *s = SX128X(ss);
    sx128x_load_defaults(s);
}

static void sx128x_reset(DeviceState *dev)
{
    SX128xState *s = SX128X(dev);
    sx128x_load_defaults(s);
}

static Property sx128x_properties[] = {
    DEFINE_PROP_UINT8("spi_id", SX128xState, spi_id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void sx128x_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *ssc = SSI_PERIPHERAL_CLASS(klass);

    ssc->realize = sx128x_realize;
    ssc->transfer = sx128x_transfer;
    ssc->set_cs = sx128x_set_cs;
    ssc->cs_polarity = SSI_CS_LOW;
    device_class_set_legacy_reset(dc, sx128x_reset);
    device_class_set_props(dc, sx128x_properties);
}

static void sx128x_instance_init(Object *obj)
{
    SX128xState *s = SX128X(obj);
    qdev_init_gpio_out(DEVICE(s), s->dio, SX128X_DIO_COUNT);
}

static const TypeInfo sx128x_info = {
    .name          = TYPE_SX128X,
    .parent        = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(SX128xState),
    .instance_init = sx128x_instance_init,
    .class_init    = sx128x_class_init,
};

static void sx128x_register_types(void)
{
    type_register_static(&sx128x_info);
}

type_init(sx128x_register_types)
