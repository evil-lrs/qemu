#ifndef HW_SSI_SX128X_H
#define HW_SSI_SX128X_H

#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_SX128X "sx128x"
OBJECT_DECLARE_SIMPLE_TYPE(SX128xState, SX128X)

/*
 * Minimal Semtech SX128x (2.4 GHz LoRa/FLRC) SSI peripheral stub.
 *
 * Unlike SX127x, SX128x has an opcode-driven SPI protocol (no register-mapped
 * SPI). The host first clocks an opcode byte, then opcode-specific
 * parameters/data, then optionally reads back. See Semtech SX1280 datasheet
 * sec. 11.
 *
 * This stub responds to enough opcodes for guest probing + a single TX:
 *   GetStatus, WriteRegister, ReadRegister, WriteBuffer, ReadBuffer,
 *   SetSleep, SetStandby, SetFs, SetTx, SetRx, SetRfFrequency,
 *   SetPacketType, SetPacketParams, SetModulationParams, SetBufferBaseAddress,
 *   GetIrqStatus, ClearIrqStatus, GetPacketStatus, SetDioIrqParams.
 * Anything else is logged and consumed (returns 0).
 */

#define SX128X_FIFO_SIZE  256
#define SX128X_REG_BYTES  0x1000  /* sparse-ish, enough for status + version */
#define SX128X_DIO_COUNT  3       /* DIO1, DIO2, DIO3 (DIO0 unused on SX128x) */

typedef enum {
    SX128X_STATE_IDLE,
    SX128X_STATE_PARAMS,    /* clocking opcode parameters in */
    SX128X_STATE_REG_DATA,  /* WriteRegister data phase */
    SX128X_STATE_REG_READ,  /* ReadRegister data phase (after 1 NOP) */
    SX128X_STATE_BUF_DATA,  /* WriteBuffer data phase */
    SX128X_STATE_BUF_READ,  /* ReadBuffer data phase (after 1 NOP) */
    SX128X_STATE_DRAIN,     /* opcode handled; clock further bytes as status */
} SX128xPhase;

typedef struct SX128xState {
    SSIPeripheral parent_obj;

    /* SPI protocol state */
    bool selected;
    SX128xPhase phase;
    uint8_t opcode;
    uint8_t param_buf[16];      /* up to 16 param bytes per opcode (largest seen ~9) */
    uint8_t param_pos;
    uint8_t param_needed;
    uint16_t reg_addr;          /* current 16-bit register address (SX128x reg space) */
    uint8_t buf_addr;           /* current data-buffer offset */
    uint16_t io_remaining;      /* data bytes left in REG_DATA / BUF_DATA phase */
    bool nop_consumed;          /* true once the post-address NOP was clocked */

    /* Chip state */
    uint8_t status;             /* status byte clocked back on every byte */
    uint8_t regs[SX128X_REG_BYTES];
    uint8_t buf[SX128X_FIFO_SIZE];
    uint8_t buf_tx_base;
    uint8_t buf_rx_base;
    uint16_t irq_status;        /* 16-bit IRQ status word */
    uint16_t irq_mask;
    uint16_t irq_dio1;
    uint16_t irq_dio2;
    uint16_t irq_dio3;

    /* Last applied modulation/packet params (for diagnostic logging only) */
    uint8_t packet_type;
    uint32_t rf_freq_raw;       /* raw 4-byte freq value */
    uint8_t packet_params[7];
    uint8_t modulation_params[3];
    uint8_t tx_payload_len;     /* extracted from SetPacketParams[2] */

    /* Wiring */
    uint8_t spi_id;
    bool dio_level[SX128X_DIO_COUNT];
    qemu_irq dio[SX128X_DIO_COUNT];
} SX128xState;

#endif /* HW_SSI_SX128X_H */
