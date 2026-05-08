#pragma once

#include "hw/ssi/ssi.h"

#define TYPE_SX127X "sx127x"
OBJECT_DECLARE_SIMPLE_TYPE(SX127xState, SX127X)

typedef struct SX127xState {
    SSIPeripheral parent_obj;

    uint8_t regs[128];

    bool have_addr;
    bool is_write;
    bool selected;
    uint8_t addr;

    uint8_t fifo[256];
    uint8_t fifo_pos;
    uint8_t spi_id;

    qemu_irq dio[6];
} SX127xState;
