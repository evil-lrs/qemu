#ifndef HW_SSI_LR1121_H
#define HW_SSI_LR1121_H

#include "hw/ssi/ssi.h"
#include "hw/irq.h"
#include "hw/ssi/semtech_radio_common.h"
#include "qom/object.h"

#define TYPE_LR1121 "lr1121"
#define LR1121_DIO_GPIO "dio"
#define LR1121_BUSY_GPIO "busy"
OBJECT_DECLARE_SIMPLE_TYPE(LR1121State, LR1121)

#define LR1121_DIO_COUNT 3
#define LR1121_REG_BYTES 0x10000
#define LR1121_BUF_BYTES 256

void lr1121_linker_anchor(void);

typedef enum LR1121Phase {
    LR1121_STATE_IDLE = 0,
    LR1121_STATE_OPCODE_LSB,
    LR1121_STATE_PARAMS,
    LR1121_STATE_DRAIN,
    LR1121_STATE_MEM_DATA,
    LR1121_STATE_MEM_READ,
    LR1121_STATE_BUF_DATA,
    LR1121_STATE_BUF_READ,
} LR1121Phase;

typedef struct LR1121State {
    SSIPeripheral parent_obj;

    uint8_t spi_id;
    bool selected;

    LR1121Phase phase;

    uint16_t opcode;
    uint16_t last_read_opcode;
    uint16_t param_pos;
    uint16_t param_needed;
    uint8_t param_buf[256];

    uint8_t status1;
    uint8_t status2;

    uint32_t irq_status;
    uint32_t irq_dio1;
    uint32_t irq_dio2;
    uint32_t irq_dio3;

    uint32_t errors;

    uint8_t regs[LR1121_REG_BYTES];
    uint8_t buf[LR1121_BUF_BYTES];

    uint32_t mem_addr;
    uint16_t buf_addr;
    uint16_t read_len;
    uint16_t response_pos;

    uint32_t rf_freq_hz;
    uint8_t packet_type;

    uint8_t modulation_params[16];
    uint8_t modulation_params_len;

    uint8_t packet_params[32];
    uint8_t packet_params_len;

    uint8_t tx_params[2];

    uint8_t tx_payload_len;
    uint8_t rx_payload_len;
    uint8_t rx_start_offset;

    uint8_t lora_syncword;
    uint64_t gfsk_syncword;
    uint8_t lora_public_network;

    uint8_t fallback_mode;

    qemu_irq dio[LR1121_DIO_COUNT];
    qemu_irq busy;
    bool dio_level[LR1121_DIO_COUNT];
    bool busy_level;

    /* Air bus and logging */
    SemtechRadioAirBus air_bus;
    char *radio_id;
    char *tx_log_path;
} LR1121State;

#endif
