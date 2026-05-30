#pragma once

#include "hw/ssi/ssi.h"
#include "hw/ssi/semtech_radio_common.h"

#define TYPE_SX127X "sx127x"
#define SX127X_DIO_GPIO "dio"
OBJECT_DECLARE_SIMPLE_TYPE(SX127xState, SX127X)

void sx127x_linker_anchor(void);

typedef struct SX127xState {
    SSIPeripheral parent_obj;

    uint8_t regs[128];

    bool have_addr;
    bool is_write;
    bool selected;
    uint8_t data_count;
    uint8_t addr;

    uint8_t fifo[256];
    uint8_t fifo_pos;
    uint8_t spi_id;
    bool dio_level[6];
    int64_t trace_start_ns;
    uint32_t trace_frf;
    uint8_t trace_op_mode;
    uint8_t trace_modem_config1;
    uint8_t trace_modem_config2;
    uint8_t trace_modem_config3;
    uint8_t trace_payload_length;
    uint16_t trace_preamble_length;
    uint8_t trace_sync_word;

    qemu_irq dio[6];

    /* Air bus and logging */
    SemtechRadioAirBus air_bus;
    char *radio_id;
    char *tx_log_path;
} SX127xState;
