/*
 * ESP32 SPI controller
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/sysemu.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "hw/ssi/esp32_spi.h"
#include "hw/misc/esp32_flash_enc.h"



enum {
    CMD_RES = 0xab,
    CMD_DP = 0xb9,
    CMD_CE = 0x60,
    CMD_BE = 0xD8,
    CMD_SE = 0x20,
    CMD_PP = 0x02,
    CMD_WRSR = 0x1,
    CMD_RDSR = 0x5,
    CMD_RDID = 0x9f,
    CMD_WRDI = 0x4,
    CMD_WREN = 0x6,
    CMD_READ = 0x03,
};


#define ESP32_SPI_REG_SIZE    0x1000

static void esp32_spi_do_command(Esp32SpiState* state, uint32_t cmd_reg);

static bool esp32_spi_debug(Esp32SpiState *s)
{
    return s->id >= 1;
}

static const char *esp32_spi_reg_name(hwaddr addr)
{
    switch (addr) {
    case A_SPI_CMD: return "CMD";
    case A_SPI_ADDR: return "ADDR";
    case A_SPI_CTRL: return "CTRL";
    case A_SPI_STATUS: return "STATUS";
    case A_SPI_CTRL1: return "CTRL1";
    case A_SPI_CTRL2: return "CTRL2";
    case A_SPI_USER: return "USER";
    case A_SPI_USER1: return "USER1";
    case A_SPI_USER2: return "USER2";
    case A_SPI_MOSI_DLEN: return "MOSI_DLEN";
    case A_SPI_MISO_DLEN: return "MISO_DLEN";
    case A_SPI_PIN: return "PIN";
    case A_SPI_SLAVE: return "SLAVE";
    case A_SPI_EXT0: return "EXT0";
    case A_SPI_EXT1: return "EXT1";
    case A_SPI_EXT2: return "EXT2";
    case A_SPI_EXT3: return "EXT3";
    default:
        if (addr >= A_SPI_W0 &&
            addr <= A_SPI_W0 + (ESP32_SPI_BUF_WORDS - 1) * sizeof(uint32_t)) {
            return "Wn";
        }
        return NULL;
    }
}

static void esp32_spi_append_phase(GString *str, const void *buf,
                                   int tx_bytes, int rx_bytes)
{
    const uint8_t *bytes = buf;
    int count = MAX(tx_bytes, rx_bytes);

    for (int i = 0; i < count; ++i) {
        uint8_t byte = 0;

        if (bytes && i < tx_bytes) {
            byte = bytes[i];
        }
        g_string_append_printf(str, "%s%02x", str->len ? " " : "", byte);
    }
}

static void esp32_spi_append_rx_phase(GString *str, const void *buf,
                                      int tx_bytes, int rx_bytes)
{
    const uint8_t *bytes = buf;
    int count = MAX(tx_bytes, rx_bytes);

    for (int i = 0; i < count; ++i) {
        uint8_t byte = 0;

        if (bytes && i < rx_bytes) {
            byte = bytes[i];
        }
        g_string_append_printf(str, "%s%02x", str->len ? " " : "", byte);
    }
}

static uint64_t esp32_spi_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32SpiState *s = ESP32_SPI(opaque);
    uint64_t r = 0;
    switch (addr) {
    case A_SPI_CMD:
        r = s->cmd_reg;
        if (s->id == 2 || s->id == 3) {
            fprintf(stderr, "ESP32_SPI%d: read CMD -> 0x%08x\n", s->id, (uint32_t)r);
        }
        break;
    case A_SPI_ADDR:
        r = s->addr_reg;
        break;
    case A_SPI_CTRL:
        r = s->ctrl_reg;
        break;
    case A_SPI_STATUS:
        r = s->status_reg;
        break;
    case A_SPI_CTRL1:
        r = s->ctrl1_reg;
        break;
    case A_SPI_CTRL2:
        r = s->ctrl2_reg;
        break;
    case A_SPI_USER:
        r = s->user_reg;
        break;
    case A_SPI_USER1:
        r = s->user1_reg;
        break;
    case A_SPI_USER2:
        r = s->user2_reg;
        break;
    case A_SPI_MOSI_DLEN:
        r = s->mosi_dlen_reg;
        break;
    case A_SPI_MISO_DLEN:
        r = s->miso_dlen_reg;
        break;
    case A_SPI_PIN:
        r = s->pin_reg;
        break;
    case A_SPI_W0 ... A_SPI_W0 + (ESP32_SPI_BUF_WORDS - 1) * sizeof(uint32_t):
        r = s->data_reg[(addr - A_SPI_W0) / sizeof(uint32_t)];
        break;
    case A_SPI_EXT2:
        r = 0;
        break;
    case A_SPI_SLAVE:
        r = BIT(R_SPI_SLAVE_TRANS_DONE_SHIFT) | BIT(R_SPI_SLAVE_TRANS_INTEN_SHIFT);
        break;
    default:
        if (esp32_spi_debug(s)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ESP32_SPI%d: read unknown offset=0x%" HWADDR_PRIx
                          " size=%u\n", s->id, addr, size);
        }
        break;
    }
    if (esp32_spi_debug(s)) {
        const char *name = esp32_spi_reg_name(addr);

        if (name) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ESP32_SPI%d: read %s(0x%" HWADDR_PRIx
                          ") -> 0x%" PRIx64 "\n",
                          s->id, name, addr, r);
        }
    }
    return r;
}

static void esp32_spi_write(void *opaque, hwaddr addr,
                       uint64_t value, unsigned int size)
{
    Esp32SpiState *s = ESP32_SPI(opaque);
    switch (addr) {
    case A_SPI_W0 ... A_SPI_W0 + (ESP32_SPI_BUF_WORDS - 1) * sizeof(uint32_t): {
        int idx = (addr - A_SPI_W0) / sizeof(uint32_t);
        s->data_reg[idx] = value;
        if (esp32_spi_debug(s) && idx == 0) {
            uint8_t bytes[4];
            memcpy(bytes, &s->data_reg[0], 4);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ESP32_SPI%d: W0 mem=[%02x %02x %02x %02x]\n",
                          s->id, bytes[0], bytes[1], bytes[2], bytes[3]);
        }
        break;
    }
    case A_SPI_ADDR:
        s->addr_reg = value;
        break;
    case A_SPI_CTRL:
        s->ctrl_reg = value;
        break;
    case A_SPI_STATUS:
        s->status_reg = value;
        break;
    case A_SPI_CTRL1:
        s->ctrl1_reg = value;
        break;
    case A_SPI_CTRL2:
        s->ctrl2_reg = value;
        break;
    case A_SPI_USER:
        s->user_reg = value;
        break;
    case A_SPI_USER1:
        s->user1_reg = value;
        break;
    case A_SPI_USER2:
        s->user2_reg = value;
        break;
    case A_SPI_MOSI_DLEN:
        s->mosi_dlen_reg = value;
        break;
    case A_SPI_MISO_DLEN:
        s->miso_dlen_reg = value;
        break;
    case A_SPI_PIN:
        s->pin_reg = value;
        break;
    case A_SPI_CMD:
        if (esp32_spi_debug(s)) {
            fprintf(stderr, "ESP32_SPI%d: CMD write 0x%08x\n", s->id, (uint32_t)value);
        }
        s->cmd_reg = value;
        esp32_spi_do_command(s, value);
        break;
    default:
        if (esp32_spi_debug(s)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ESP32_SPI%d: write unknown offset=0x%" HWADDR_PRIx
                          " value=0x%" PRIx64 " size=%u\n",
                          s->id, addr, value, size);
        }
        break;
    }
    if (esp32_spi_debug(s)) {
        const char *name = esp32_spi_reg_name(addr);

        if (name) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ESP32_SPI%d: write %s(0x%" HWADDR_PRIx
                          ") = 0x%" PRIx64 "\n",
                          s->id, name, addr, value);
        }
    }
}

typedef struct Esp32SpiTransaction {
    int cmd_bytes;
    uint32_t cmd;
    int addr_bytes;
    uint32_t addr;
    int data_tx_bytes;
    int data_rx_bytes;
    uint32_t* data;
} Esp32SpiTransaction;

static void esp32_spi_txrx_buffer(Esp32SpiState *s, const char *phase, void *buf, int tx_bytes, int rx_bytes)
{
    int bytes = MAX(tx_bytes, rx_bytes);
    uint8_t *c_buf = (uint8_t*) buf;

    if (esp32_spi_debug(s)) {
        uint32_t first_word = 0;
        if (buf && bytes >= 4) {
            memcpy(&first_word, buf, 4);
        } else if (buf && bytes > 0) {
            memcpy(&first_word, buf, bytes);
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ESP32_SPI%d: %s start len=%d tx=%d rx=%d word0=0x%08x\n",
                      s->id, phase, bytes, tx_bytes, rx_bytes, first_word);
    }

    for (int i = 0; i < bytes; ++i) {
        uint8_t byte = 0;
        if (c_buf && i < tx_bytes) {
            byte = c_buf[i];
        }

        uint32_t res = ssi_transfer(s->spi, byte);

        if (esp32_spi_debug(s) && (i < 8 || i == bytes - 1)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ESP32_SPI%d:   %s[%d] tx=0x%02x rx=0x%02x\n",
                          s->id, phase, i, byte, res);
        }

        if (c_buf && i < rx_bytes) {
            c_buf[i] = res;
        }
    }
}

static void esp32_spi_cs_set(Esp32SpiState *s, int value)
{
    for (int i = 0; i < ESP32_SPI_CS_COUNT; ++i) {
        int level;

        if (s->external_cs_valid[i]) {
            level = s->external_cs_level[i];
        } else {
            level = ((s->pin_reg & (1 << i)) == 0) ? value : 1;
        }
        qemu_set_irq(s->cs_gpio[i], level);
    }
}

static void esp32_spi_external_cs(void *opaque, int n, int level)
{
    Esp32SpiState *s = ESP32_SPI(opaque);

    if (n < 0 || n >= ESP32_SPI_CS_COUNT) {
        return;
    }

    s->external_cs_valid[n] = true;
    s->external_cs_level[n] = level ? 1 : 0;

    if (esp32_spi_debug(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ESP32_SPI%d: external CS%d %s\n",
                      s->id, n, level ? "high" : "low");
    }

    /* Forward the GPIO level to the SSI device CS line */
    qemu_set_irq(s->cs_gpio[n], s->external_cs_level[n]);
}

static void esp32_spi_transaction(Esp32SpiState *s, Esp32SpiTransaction *t)
{
    GString *mosi = NULL;
    GString *miso = NULL;

    if (esp32_spi_debug(s)) {
        mosi = g_string_new(NULL);
        esp32_spi_append_phase(mosi, &t->cmd, t->cmd_bytes, 0);
        esp32_spi_append_phase(mosi, &t->addr, t->addr_bytes, 0);
        esp32_spi_append_phase(mosi, t->data, t->data_tx_bytes,
                               t->data_rx_bytes);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ESP32_SPI%d: transaction mosi_len=%d miso_len=%d "
                      "MOSI=[%s]\n",
                      s->id,
                      t->cmd_bytes + t->addr_bytes +
                      MAX(t->data_tx_bytes, t->data_rx_bytes),
                      t->data_rx_bytes,
                      mosi->str);
        g_string_free(mosi, true);
    }

    esp32_spi_cs_set(s, 0);
    esp32_spi_txrx_buffer(s, "cmd", &t->cmd, t->cmd_bytes, 0);
    esp32_spi_txrx_buffer(s, "addr", &t->addr, t->addr_bytes, 0);
    esp32_spi_txrx_buffer(s, "data", t->data, t->data_tx_bytes, t->data_rx_bytes);
    esp32_spi_cs_set(s, 1);

    if (esp32_spi_debug(s)) {
        miso = g_string_new(NULL);
        esp32_spi_append_rx_phase(miso, &t->cmd, t->cmd_bytes, 0);
        esp32_spi_append_rx_phase(miso, &t->addr, t->addr_bytes, 0);
        esp32_spi_append_rx_phase(miso, t->data, t->data_tx_bytes,
                                  t->data_rx_bytes);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ESP32_SPI%d: transaction MISO=[%s]\n",
                      s->id, miso->str);
        g_string_free(miso, true);
    }
}

/* Convert one of the hardware "bitlen" registers to a byte count */
static inline int bitlen_to_bytes(uint32_t val)
{
    return (val + 1 + 7) / 8; /* bitlen registers hold number of bits, minus one */
}

static void maybe_encrypt_data(Esp32SpiState *s)
{
    Esp32FlashEncryptionState* flash_enc = esp32_flash_encryption_find();
    if (esp32_flash_encryption_enabled(flash_enc)) {
        esp32_flash_encryption_get_result(flash_enc, &s->data_reg[0], 8);
    }
}

static void esp32_spi_do_command(Esp32SpiState* s, uint32_t cmd_reg)
{
    Esp32SpiTransaction t = {
        .cmd_bytes = 0,
        .addr_bytes = 0,
        .data_tx_bytes = 0,
        .data_rx_bytes = 0,
        .data = NULL
    };
    switch (cmd_reg) {
    case R_SPI_CMD_READ_MASK:
        t.cmd = CMD_READ;
        t.cmd_bytes = 1;
        t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
        t.addr = s->addr_reg;
        /* SPI Flash (SPI1) expects addresses MSB-first.
         * Reverse only when the driver is reading. */
        if (s->id == 1) {
            if (t.addr_bytes == 3) {
                t.addr = ((s->addr_reg & 0x0000ff) << 16) |
                         (s->addr_reg & 0x00ff00) |
                         ((s->addr_reg & 0xff0000) >> 16);
            } else if (t.addr_bytes == 4) {
                t.addr = ((s->addr_reg & 0x000000ff) << 24) |
                         ((s->addr_reg & 0x0000ff00) << 8) |
                         ((s->addr_reg & 0x00ff0000) >> 8) |
                         ((s->addr_reg & 0xff000000) >> 24);
            }
        }
        t.data = &s->data_reg[0];
        t.data_rx_bytes = bitlen_to_bytes(s->miso_dlen_reg);
        break;

    case R_SPI_CMD_WREN_MASK:
        t.cmd = CMD_WREN;
        t.cmd_bytes = 1;
        break;

    case R_SPI_CMD_WRDI_MASK:
        t.cmd = CMD_WRDI;
        t.cmd_bytes = 1;
        break;

    case R_SPI_CMD_RDID_MASK:
        t.cmd = CMD_RDID;
        t.cmd_bytes = 1;
        t.data = &s->data_reg[0];
        t.data_rx_bytes = 3;
        break;

    case R_SPI_CMD_RDSR_MASK:
        t.cmd = CMD_RDSR;
        t.cmd_bytes = 1;
        t.data = &s->status_reg;
        t.data_rx_bytes = 1;
        break;

    case R_SPI_CMD_WRSR_MASK:
        t.cmd = CMD_WRSR;
        t.cmd_bytes = 1;
        t.data = &s->status_reg;
        t.data_tx_bytes = 1;
        break;

    case R_SPI_CMD_PP_MASK:
        maybe_encrypt_data(s);
        t.cmd = CMD_PP;
        t.cmd_bytes = 1;
        t.data = &s->data_reg[0];
        t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
        t.addr = s->addr_reg;
        t.data = &s->data_reg[0];
        t.data_tx_bytes = s->addr_reg >> 24;
        break;

    case R_SPI_CMD_SE_MASK:
        t.cmd = CMD_SE;
        t.cmd_bytes = 1;
        t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
        t.addr = s->addr_reg;
        break;

    case R_SPI_CMD_BE_MASK:
        t.cmd = CMD_BE;
        t.cmd_bytes = 1;
        t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
        t.addr = s->addr_reg;
        break;

    case R_SPI_CMD_CE_MASK:
        t.cmd = CMD_CE;
        t.cmd_bytes = 1;
        break;

    case R_SPI_CMD_DP_MASK:
        t.cmd = CMD_DP;
        t.cmd_bytes = 1;
        break;

    case R_SPI_CMD_RES_MASK:
        t.cmd = CMD_RES;
        t.cmd_bytes = 1;
        t.data = &s->data_reg[0];
        t.data_rx_bytes = 3;
        break;

    case R_SPI_CMD_USR_MASK:
        maybe_encrypt_data(s);
        if (FIELD_EX32(s->user_reg, SPI_USER, COMMAND)) {
            t.cmd = FIELD_EX32(s->user2_reg, SPI_USER2, COMMAND_VALUE);
            t.cmd_bytes = bitlen_to_bytes(FIELD_EX32(s->user2_reg, SPI_USER2, COMMAND_BITLEN));
        } else {
            t.cmd_bytes = 0;
        }
        if (FIELD_EX32(s->user_reg, SPI_USER, ADDR)) {
            t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
            t.addr = s->addr_reg;
        } else {
            t.addr_bytes = 0;
        }
        if (FIELD_EX32(s->user_reg, SPI_USER, MOSI)) {
            t.data = &s->data_reg[0];
            t.data_tx_bytes = bitlen_to_bytes(s->mosi_dlen_reg);
        }
        if (FIELD_EX32(s->user_reg, SPI_USER, MISO)) {
            t.data = &s->data_reg[0];
            t.data_rx_bytes = bitlen_to_bytes(s->miso_dlen_reg);
        }
        break;
    default:
        s->cmd_reg = 0;
        return;
    }
    esp32_spi_transaction(s, &t);
    s->cmd_reg = 0;
}


static const MemoryRegionOps esp32_spi_ops = {
    .read =  esp32_spi_read,
    .write = esp32_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_spi_reset_hold(Object *obj, ResetType type)
{
    Esp32SpiState *s = ESP32_SPI(obj);
    s->pin_reg = 0x6;
    s->user1_reg = FIELD_DP32(0, SPI_USER1, ADDR_BITLEN, 23);
    s->user1_reg = FIELD_DP32(s->user1_reg, SPI_USER1, DUMMY_CYCLELEN, 7);
    s->user2_reg = FIELD_DP32(0, SPI_USER2, COMMAND_BITLEN, 4);
    s->user2_reg = FIELD_DP32(s->user2_reg, SPI_USER2, COMMAND_VALUE, 0);
    s->status_reg = 0;
    memset(s->external_cs_valid, 0, sizeof(s->external_cs_valid));
    for (int i = 0; i < ESP32_SPI_CS_COUNT; ++i) {
        s->external_cs_level[i] = 1;
        qemu_set_irq(s->cs_gpio[i], 1);
    }
}

static void esp32_spi_realize(DeviceState *dev, Error **errp)
{
}

static void esp32_spi_init(Object *obj)
{
    Esp32SpiState *s = ESP32_SPI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32_spi_ops, s,
                          TYPE_ESP32_SPI, ESP32_SPI_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->spi = ssi_create_bus(DEVICE(s), "spi");
    qdev_init_gpio_out_named(DEVICE(s), &s->cs_gpio[0], SSI_GPIO_CS, ESP32_SPI_CS_COUNT);
    qdev_init_gpio_in_named(DEVICE(s), esp32_spi_external_cs,
                            ESP32_SPI_EXTERNAL_CS_GPIO, ESP32_SPI_CS_COUNT);
}

static Property esp32_spi_properties[] = {
    DEFINE_PROP_INT32("id", Esp32SpiState, id, -1),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32_spi_reset_hold;
    dc->realize = esp32_spi_realize;
    device_class_set_props(dc, esp32_spi_properties);
}

static const TypeInfo esp32_spi_info = {
    .name = TYPE_ESP32_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32SpiState),
    .instance_init = esp32_spi_init,
    .class_init = esp32_spi_class_init
};

static void esp32_spi_register_types(void)
{
    type_register_static(&esp32_spi_info);
}

type_init(esp32_spi_register_types)
