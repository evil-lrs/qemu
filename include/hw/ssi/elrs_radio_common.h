#ifndef HW_SSI_ELRS_RADIO_COMMON_H
#define HW_SSI_ELRS_RADIO_COMMON_H

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/ssi/semtech_radio_common.h"

typedef enum ElrsRadioKind {
    ELRS_RADIO_KIND_SX127X = 0,
    ELRS_RADIO_KIND_SX128X = 1,
    ELRS_RADIO_KIND_LR1121 = 2,
} ElrsRadioKind;

typedef struct ElrsRadioCommonState {
    ElrsRadioKind kind;
    uint32_t index;

    bool selected;

    qemu_irq dio0;
    qemu_irq dio1;
    qemu_irq busy;

    uint8_t fifo[512];
    uint16_t fifo_len;
    uint16_t fifo_read_pos;
    uint16_t fifo_write_pos;

    SemtechRadioAirBus air_bus;
    char *radio_id;
    char *tx_log_path;
} ElrsRadioCommonState;

const char *elrs_radio_kind_name(ElrsRadioKind kind);

void elrs_radio_common_reset(ElrsRadioCommonState *s, ElrsRadioKind kind, uint32_t index);

void elrs_radio_log_probe(ElrsRadioCommonState *s, const char *message);
void elrs_radio_log_reg_read(ElrsRadioCommonState *s, uint32_t reg, uint32_t value);
void elrs_radio_log_reg_write(ElrsRadioCommonState *s, uint32_t reg, uint32_t value);
void elrs_radio_log_fifo_tx(ElrsRadioCommonState *s, const uint8_t *data, size_t len);
void elrs_radio_log_irq(ElrsRadioCommonState *s, const char *irq_name, bool level);

void elrs_radio_fifo_clear(ElrsRadioCommonState *s);
bool elrs_radio_fifo_push(ElrsRadioCommonState *s, uint8_t value);
bool elrs_radio_fifo_pop(ElrsRadioCommonState *s, uint8_t *value);

void elrs_radio_set_dio0(ElrsRadioCommonState *s, bool level);
void elrs_radio_set_dio1(ElrsRadioCommonState *s, bool level);
void elrs_radio_set_busy(ElrsRadioCommonState *s, bool level);

#endif
