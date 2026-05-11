#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/ssi/elrs_radio_common.h"

const char *elrs_radio_kind_name(ElrsRadioKind kind)
{
    switch (kind) {
    case ELRS_RADIO_KIND_SX127X: return "sx127x";
    case ELRS_RADIO_KIND_SX128X: return "sx128x";
    case ELRS_RADIO_KIND_LR1121: return "lr1121";
    default: return "unknown";
    }
}

void elrs_radio_common_reset(ElrsRadioCommonState *s, ElrsRadioKind kind, uint32_t index)
{
    s->kind = kind;
    s->index = index;
    s->selected = false;
    elrs_radio_fifo_clear(s);
}

void elrs_radio_log_probe(ElrsRadioCommonState *s, const char *message)
{
    qemu_log("ELRS-RADIO[%d]: kind=%s probe %s\n",
             s->index, elrs_radio_kind_name(s->kind), message);
}

void elrs_radio_log_reg_read(ElrsRadioCommonState *s, uint32_t reg, uint32_t value)
{
    qemu_log("ELRS-RADIO[%d]: kind=%s reg read addr=0x%02x -> 0x%02x\n",
             s->index, elrs_radio_kind_name(s->kind), reg, value);
}

void elrs_radio_log_reg_write(ElrsRadioCommonState *s, uint32_t reg, uint32_t value)
{
    qemu_log("ELRS-RADIO[%d]: kind=%s reg write addr=0x%02x value=0x%02x\n",
             s->index, elrs_radio_kind_name(s->kind), reg, value);
}

void elrs_radio_log_fifo_tx(ElrsRadioCommonState *s, const uint8_t *data, size_t len)
{
    qemu_log("ELRS-RADIO[%d]: kind=%s TX len=%zu\n",
             s->index, elrs_radio_kind_name(s->kind), len);
}

void elrs_radio_log_irq(ElrsRadioCommonState *s, const char *irq_name, bool level)
{
    qemu_log("ELRS-RADIO[%d]: kind=%s IRQ %s=%d\n",
             s->index, elrs_radio_kind_name(s->kind), irq_name, level);
}

void elrs_radio_fifo_clear(ElrsRadioCommonState *s)
{
    s->fifo_len = 0;
    s->fifo_read_pos = 0;
    s->fifo_write_pos = 0;
}

bool elrs_radio_fifo_push(ElrsRadioCommonState *s, uint8_t value)
{
    if (s->fifo_len < sizeof(s->fifo)) {
        s->fifo[s->fifo_write_pos] = value;
        s->fifo_write_pos = (s->fifo_write_pos + 1) % sizeof(s->fifo);
        s->fifo_len++;
        return true;
    }
    return false;
}

bool elrs_radio_fifo_pop(ElrsRadioCommonState *s, uint8_t *value)
{
    if (s->fifo_len > 0) {
        *value = s->fifo[s->fifo_read_pos];
        s->fifo_read_pos = (s->fifo_read_pos + 1) % sizeof(s->fifo);
        s->fifo_len--;
        return true;
    }
    return false;
}

void elrs_radio_set_dio0(ElrsRadioCommonState *s, bool level)
{
    if (s->dio0) {
        qemu_set_irq(s->dio0, level);
        elrs_radio_log_irq(s, "dio0", level);
    }
}

void elrs_radio_set_dio1(ElrsRadioCommonState *s, bool level)
{
    if (s->dio1) {
        qemu_set_irq(s->dio1, level);
        elrs_radio_log_irq(s, "dio1", level);
    }
}

void elrs_radio_set_busy(ElrsRadioCommonState *s, bool level)
{
    if (s->busy) {
        qemu_set_irq(s->busy, level);
        elrs_radio_log_irq(s, "busy", level);
    }
}
