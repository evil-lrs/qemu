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
#include "hw/irq.h"

#define SX127X_REG_FIFO        0x00
#define SX127X_REG_OP_MODE     0x01
#define SX127X_REG_FIFO_ADDR_PTR 0x0d
#define SX127X_REG_FIFO_TX_BASE_ADDR 0x0e
#define SX127X_REG_FIFO_RX_BASE_ADDR 0x0f
#define SX127X_REG_FIFO_RX_CURRENT_ADDR 0x10
#define SX127X_REG_IRQ_FLAGS   0x12
#define SX127X_REG_RX_NB_BYTES 0x13
#define SX127X_REG_PAYLOAD_LENGTH 0x22
#define SX127X_REG_DIO_MAPPING1 0x40
#define SX127X_REG_VERSION     0x42
#define SX127X_IRQ_TX_DONE     0x08

static const char *sx127x_reg_name(uint8_t addr)
{
    switch (addr) {
    case SX127X_REG_FIFO:
        return "RegFifo";
    case SX127X_REG_OP_MODE:
        return "RegOpMode";
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
    s->regs[0x1e] = 0x74; /* RegModemConfig2 */
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
    s->addr = 0;
    s->fifo_pos = 0;
    memset(s->dio_level, 0, sizeof(s->dio_level));
}

static int sx127x_set_cs(SSIPeripheral *ss, bool select)
{
    SX127xState *s = SX127X(ss);
    bool is_selected = (select == (ss->spc->cs_polarity == SSI_CS_HIGH));

    qemu_log_mask(LOG_GUEST_ERROR, "SX127X[SPI%d:CS%d]: set_cs level=%d (selected=%d, have_addr=%d)\n",
                  s->spi_id, s->parent_obj.cs_index, select, is_selected, s->have_addr);

    if (is_selected && !s->selected) {
        s->have_addr = false;
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
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SX127X[SPI%d:CS%d]: DIO0 %s irq=0x%02x\n",
                      s->spi_id, s->parent_obj.cs_index,
                      irq ? "high" : "low", s->regs[SX127X_REG_IRQ_FLAGS]);
        s->dio_level[0] = irq;
    }
    qemu_set_irq(s->dio[0], irq);
}

static void sx127x_log_tx_payload(SX127xState *s)
{
    uint8_t base = s->regs[SX127X_REG_FIFO_TX_BASE_ADDR];
    uint8_t len = s->regs[SX127X_REG_PAYLOAD_LENGTH];
    char payload[3 * 256 + 1];
    size_t off = 0;
    int i;

    for (i = 0; i < len && off < sizeof(payload); i++) {
        off += snprintf(payload + off, sizeof(payload) - off, "%s%02x",
                        i == 0 ? "" : " ", s->fifo[(uint8_t)(base + i)]);
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "SX127X[SPI%d:CS%d]: TX len=%u freq=%02x%02x%02x "
                  "modem1=%02x modem2=%02x payload=[%s]\n",
                  s->spi_id, s->parent_obj.cs_index, len,
                  s->regs[0x06], s->regs[0x07], s->regs[0x08],
                  s->regs[0x1d], s->regs[0x1e], payload);
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
    if (addr == SX127X_REG_FIFO) {
        s->fifo[s->fifo_pos++] = value;
        s->regs[SX127X_REG_FIFO_ADDR_PTR] = s->fifo_pos;
    } else if (addr == SX127X_REG_IRQ_FLAGS) {
        s->regs[SX127X_REG_IRQ_FLAGS] &= ~value;
    } else if (addr == SX127X_REG_OP_MODE) {
        s->regs[addr & 0x7f] = value;
        if ((value & 0x07) == 0x03) { /* TX mode */
            sx127x_log_tx_payload(s);
            /* Immediately trigger TX_DONE in RegIrqFlags */
            s->regs[SX127X_REG_IRQ_FLAGS] |= SX127X_IRQ_TX_DONE;
        }
    } else {
        s->regs[addr & 0x7f] = value;
        if (addr == SX127X_REG_FIFO_ADDR_PTR) {
            s->fifo_pos = value;
        }
    }

    sx127x_update_irq(s);

    uint8_t radio_id = s->parent_obj.cs_index;

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

    qemu_log_mask(LOG_GUEST_ERROR, "SX127X[SPI%d:CS%d]: transfer byte=0x%02x (have_addr=%d, addr=0x%02x, is_write=%d)\n",
                  s->spi_id, radio_id, byte, s->have_addr, s->addr, s->is_write);

    if (!s->have_addr) {
        s->is_write = (byte & 0x80) != 0;
        s->addr = byte & 0x7f;
        s->have_addr = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SX127X[SPI%d:CS%d]: command addr=0x%02x %s\n",
                      s->spi_id, radio_id, s->addr, s->is_write ? "write" : "read");
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
    return ret;
}

static void sx127x_realize(SSIPeripheral *ss, Error **errp)
{
    SX127xState *s = SX127X(ss);

    sx127x_load_defaults(s);
}

static void sx127x_reset(DeviceState *dev)
{
    SX127xState *s = SX127X(dev);

    sx127x_load_defaults(s);
}

static Property sx127x_properties[] = {
    DEFINE_PROP_UINT8("spi_id", SX127xState, spi_id, 0),
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

    qdev_init_gpio_out(DEVICE(s), s->dio, 6);
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
