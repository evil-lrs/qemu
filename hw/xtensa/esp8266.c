/*
 * ESP8266 SoC and machine
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/xtensa/xtensa_memory.h"
#include "hw/misc/unimp.h"
#include "hw/misc/esp_radio_config.h"
#include "hw/misc/esp_radio_board.h"
#include "hw/ssi/sx127x.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/xtensa/esp8266.h"
#include "core-lx106/core-isa.h"
#include "chardev/char-fe.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "sysemu/cpus.h"
#include "sysemu/runstate.h"
#include "sysemu/blockdev.h"
#include "sysemu/block-backend.h"
#include "exec/exec-all.h"
#include "exec/cpu-common.h"
#include "exec/address-spaces.h"
#include "elf.h"

#define TYPE_ESP8266_CPU XTENSA_CPU_TYPE_NAME("lx106")
#define ESP8266_FLASH_BASE 0x40200000
#define ESP8266_FLASH_SIZE (1 * MiB)
#define ESP8266_FLASH_MAP_SIZE (4 * MiB)
#define ESP8266_DRAM_BASE 0x3ffe8000
#define ESP8266_DRAM_SIZE (96 * KiB)
#define ESP8266_SRAM_BASE 0x3ff00000
#define ESP8266_SRAM_SIZE (256 * KiB)
#define ESP8266_ROM_BASE 0x40000000
#define ESP8266_ROM_SIZE (128 * KiB)
#define ESP8266_ROM_RESET_VECTOR 0x40000080
#define ESP8266_ROM_CACHE_READ_ENABLE 0x4000242c
#define ESP8266_ROM_ETS_VPRINTF 0x40001f00
#define ESP8266_ROM_ETS_MEMSET 0x400018a4
#define ESP8266_ROM_ETS_MEMCPY 0x400018b4
#define ESP8266_ROM_ETS_MEMCMP 0x400018d4
#define ESP8266_ROM_ETS_TASK 0x40000dd0
#define ESP8266_ROM_ETS_RUN 0x40000e04
#define ESP8266_ROM_ETS_POST 0x40000e24
#define ESP8266_ROM_ETS_PRINTF 0x400024cc
#define ESP8266_ROM_ETS_PUTC 0x40002be8
#define ESP8266_ROM_ETS_BZERO 0x40002ae8
#define ESP8266_ROM_ETS_STRLEN 0x40002ac8
#define ESP8266_ROM_RTC_GET_RESET_REASON 0x400025e0
#define ESP8266_ROM_SPI_READ_STATUS 0x400043c8
#define ESP8266_ROM_SPI_WRITE_STATUS 0x40004400
#define ESP8266_ROM_SPI_WRITE_ENABLE 0x4000443c
#define ESP8266_ROM_WAIT_SPI_IDLE 0x4000448c
#define ESP8266_ROM_SPI_ERASE_CHIP 0x40004984
#define ESP8266_ROM_SPI_ERASE_BLOCK 0x400049b4
#define ESP8266_ROM_SPI_ERASE_SECTOR 0x40004a00
#define ESP8266_ROM_SPI_WRITE 0x40004a4c
#define ESP8266_ROM_SPI_READ 0x40004b1c
#define ESP8266_ROM_SPI_ERASE_AREA 0x40004b44
#define ESP8266_ROM_UDIVDI3 0x4000d310
#define ESP8266_ROM_UMODDI3 0x4000d770
#define ESP8266_ROM_MUL_OVERFLOW_CHECK 0x4000dcf0
#define ESP8266_ROM_MEMCMP 0x4000dea8
#define ESP8266_ROM_MEMCPY 0x4000df48
#define ESP8266_ROM_MEMSET 0x4000e190
#define ESP8266_ROM_STRLEN 0x4000bf4c
#define ESP8266_ROM_FLASH_SECTOR_COUNT 0x4000e21c
#define ESP8266_ROM_FLASHCHIP 0x3fffc714
#define ESP8266_ROM_FLASHCHIP_DATA 0x3fffc718
#define ESP8266_ELRS_HARDWARE_TABLE 0x3fff1e00
#define ESP8266_ELRS_HARDWARE_FIELDS 113
#define ESP8266_ELRS_WIFI_DEVICE 0x3ffe8ac4
#define ESP8266_SDK_FLASH_PROBE 0x4025de2c
#define ESP8266_SDK_FLASH_ERASE_RANGE 0x40218100
#define ESP8266_SDK_HARDWARE_INIT 0x402050f4
#define ESP8266_SDK_HARDWARE_JSON_RESERVE 0x4020517d
#define ESP8266_SDK_RX_UID_LOG_CALL 0x40208937
#define ESP8266_SDK_RX_UID_LOG_STRING1 0x4020893f
#define ESP8266_SDK_RX_UID_LOG_STRING2 0x40208945
#define ESP8266_SDK_RX_UID_LOG_STRING3 0x4020894a
#define ESP8266_ELRS_DEVICES_INIT_REAL 0x40209868
#define ESP8266_ELRS_FHSS_RANDOMISE 0x402092ec
#define ESP8266_ELRS_FHSS_CONFIG_PTR 0x3fff31c0
#define ESP8266_ELRS_FHSS_DOMAIN_FCC900 0x3ffe8768
#define ESP8266_ELRS_FHSS_FREQ_SPREAD 0x3fff21ac
#define ESP8266_ELRS_FHSS_SYNC_CHANNEL 0x3fff21b8
#define ESP8266_ELRS_FHSS_PRIMARY_BAND_COUNT 0x3fff21bc
#define ESP8266_ELRS_FHSS_SEQUENCE 0x3fff21be
#define ESP8266_ELRS_SX127X_HAL_RESET_REAL 0x4020921c
#define ESP8266_ELRS_SX127X_HAL_RESET 0x4020bde0
#define ESP8266_SDK_SPI_READ_WRAPPER 0x4010928c
#define ESP8266_SDK_SPIFFS_OPEN 0x40212c78
#define ESP8266_FLASH_BLOCK_SIZE (64 * KiB)
#define ESP8266_FLASH_SECTOR_SIZE (4 * KiB)
#define ESP8266_FLASH_PAGE_SIZE 256
#define ESP8266_FLASH_STATUS_MASK 0xffff
#define ESP8266_FLASH_JEDEC_ID 0x0014409d
#define ESP8266_RESET_REASON_POWERON 0
#define ESP8266_IMAGE_MAGIC 0xe9
#define ESP8266_UART_FIFO 0x00
#define ESP8266_UART_STATUS 0x1c
#define ESP8266_SPI_CMD 0x00
#define ESP8266_SPI_ADDR 0x04
#define ESP8266_SPI_RD_STATUS 0x10
#define ESP8266_SPI_USER1 0x20
#define ESP8266_SPI_USER2 0x24
#define ESP8266_SPI_W0 0x40
#define ESP8266_SPI_STATUS 0xf8
#define ESP8266_SPI_CMD_WREN BIT(30)
#define ESP8266_SPI_CMD_RDID BIT(28)
#define ESP8266_SPI_CMD_RDSR BIT(27)
#define ESP8266_SPI_FLASH_OP_WRSR 0x01
#define ESP8266_SPI_FLASH_OP_PP 0x02
#define ESP8266_SPI_FLASH_OP_READ 0x03
#define ESP8266_SPI_FLASH_OP_RDSR1 0x05
#define ESP8266_SPI_FLASH_OP_FAST_READ 0x0b
#define ESP8266_SPI_FLASH_OP_SE 0x20
#define ESP8266_SPI_FLASH_OP_WRSR2 0x31
#define ESP8266_SPI_FLASH_OP_DOUT_READ 0x3b
#define ESP8266_SPI_FLASH_OP_RDSR2 0x35
#define ESP8266_SPI_FLASH_OP_RDID 0x9f
#define ESP8266_SPI_FLASH_OP_BE64K 0xd8
#define ESP8266_FLASH_STATUS2_QE BIT(1)
#define ESP8266_FLASH_HELPER_BASE 0x60001400
#define ESP8266_FLASH_HELPER_ERASE_SECTOR 0x00
#define ESP8266_FLASH_HELPER_WRITE_ADDR 0x04
#define ESP8266_FLASH_HELPER_WRITE_SRC 0x08
#define ESP8266_FLASH_HELPER_WRITE_LEN 0x0c
#define ESP8266_HSPI_CMD 0x00
#define ESP8266_HSPI_USER1 0x20
#define ESP8266_HSPI_W0 0x40
#define ESP8266_HSPI_USR BIT(18)
#define ESP8266_DPORT_OTP_MAC0 0x50
#define ESP8266_DPORT_OTP_MAC1 0x54
#define ESP8266_DPORT_OTP_MAC2 0x58
#define ESP8266_DPORT_OTP_MAC3 0x5c
#define ESP8266_OTP_MAC0_ESP8285_1M 0x12000010
#define ESP8266_OTP_MAC1_DEFAULT_OUI 0x0000d074
#define ESP8266_OTP_MAC2_ESP8266_SDK_ID 0x00008000
#define ESP8266_GPIO_IN 0x18
#define ESP8266_GPIO_BOOT_STRAPS ((1u << 0) | (1u << 2))
#define ESP8266_I2C_CLOCK_GATE 0x348
#define ESP8266_WIFI_BOOT_MAGIC 0x0d74
#define ESP8266_WIFI_STATUS 0x0800
#define ESP8266_WIFI_STATUS_READY (10u << 16)
#define ESP8266_WIFI_BUSY_STATUS 0x0b60
#define ESP8266_TIMER_STATUS 0x128
#define ESP8266_TIMER_STATUS_READY 0x1
#define ESP8266_TIMER_COUNT 0x04
#define ESP8266_RTC_TIMER_STATUS (ESP8266_TIMER_STATUS - 0x100)

static void esp8266_seed_elrs_fhss_state(void);

static uint64_t esp8266_dport_read(void *opaque, hwaddr addr,
                                   unsigned int size)
{
    switch (addr) {
    case ESP8266_DPORT_OTP_MAC0:
        return ESP8266_OTP_MAC0_ESP8285_1M;
    case ESP8266_DPORT_OTP_MAC1:
        return ESP8266_OTP_MAC1_DEFAULT_OUI;
    case ESP8266_DPORT_OTP_MAC2:
        return ESP8266_OTP_MAC2_ESP8266_SDK_ID;
    case ESP8266_DPORT_OTP_MAC3:
        return 0;
    default:
        return 0;
    }
}

static void esp8266_dport_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned int size)
{
}

static const MemoryRegionOps esp8266_dport_ops = {
    .read = esp8266_dport_read,
    .write = esp8266_dport_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t esp8266_uart_read(void *opaque, hwaddr addr, unsigned int size)
{
    switch (addr) {
    case ESP8266_UART_STATUS:
        /* Report TX FIFO empty. This is enough for ROM/eboot polling loops. */
        return 0;
    default:
        return 0;
    }
}

static void esp8266_uart_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr == ESP8266_UART_FIFO) {
        uint8_t ch = value & 0xff;
        if (qemu_chr_fe_backend_connected(&s->uart0_chr)) {
            qemu_chr_fe_write_all(&s->uart0_chr, &ch, 1);
        } else {
            qemu_log("%c", ch);
        }
    }
}

static const MemoryRegionOps esp8266_uart_ops = {
    .read = esp8266_uart_read,
    .write = esp8266_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void esp8266_hspi_transfer(Esp8266SocState *s)
{
    uint32_t user1 = s->hspi_regs[ESP8266_HSPI_USER1 / 4];
    uint32_t bitlen = ((user1 >> 17) & 0x1ff) + 1;
    uint32_t bytes = DIV_ROUND_UP(bitlen, 8);
    uint8_t *buf = (uint8_t *)&s->hspi_regs[ESP8266_HSPI_W0 / 4];
    uint32_t fhss_config;

    if (!s->hspi_bus || bytes == 0) {
        return;
    }

    cpu_physical_memory_read(ESP8266_ELRS_FHSS_CONFIG_PTR, &fhss_config,
                             sizeof(fhss_config));
    if (fhss_config == 0) {
        esp8266_seed_elrs_fhss_state();
    }

    if (s->hspi_cs) {
        qemu_set_irq(s->hspi_cs, 0);
    }
    for (uint32_t i = 0; i < bytes && i < 64; i++) {
        buf[i] = ssi_transfer(s->hspi_bus, buf[i]) & 0xff;
    }
    if (s->hspi_cs) {
        qemu_set_irq(s->hspi_cs, 1);
    }
}

static uint64_t esp8266_hspi_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr < sizeof(s->hspi_regs) && (addr % 4) == 0) {
        return s->hspi_regs[addr / 4];
    }
    return 0;
}

static void esp8266_hspi_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr >= sizeof(s->hspi_regs) || (addr % 4) != 0) {
        return;
    }

    if (addr == ESP8266_HSPI_CMD) {
        s->hspi_regs[addr / 4] = value;
        if (value & ESP8266_HSPI_USR) {
            esp8266_hspi_transfer(s);
        }
        s->hspi_regs[addr / 4] = 0;
        return;
    }

    s->hspi_regs[addr / 4] = value;
}

static const MemoryRegionOps esp8266_hspi_ops = {
    .read = esp8266_hspi_read,
    .write = esp8266_hspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t esp8266_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr == ESP8266_GPIO_IN) {
        return s->gpio_regs[addr / 4] | ESP8266_GPIO_BOOT_STRAPS;
    }
    if (addr < sizeof(s->gpio_regs) && (addr % 4) == 0) {
        return s->gpio_regs[addr / 4];
    }
    return 0;
}

static void esp8266_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr < sizeof(s->gpio_regs) && (addr % 4) == 0) {
        s->gpio_regs[addr / 4] = value;
    }
}

static const MemoryRegionOps esp8266_gpio_ops = {
    .read = esp8266_gpio_read,
    .write = esp8266_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t esp8266_spi_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr == ESP8266_SPI_STATUS) {
        return 0;
    }
    if (addr < sizeof(s->spi_regs) && (addr % 4) == 0) {
        return s->spi_regs[addr / 4];
    }
    return 0;
}

static void esp8266_spi_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr == ESP8266_SPI_CMD) {
        uint32_t user2 = s->spi_regs[ESP8266_SPI_USER2 / 4];
        uint8_t flash_op = user2 & 0xff;

        if (flash_op == 0) {
            flash_op = extract32(user2, 24, 8);
        }

        /*
         * SDK startup issues short SPI flash commands and then polls SPI_CMD
         * until the command bits self-clear. Complete them synchronously.
         */
        if (value & ESP8266_SPI_CMD_WREN) {
            s->spi_regs[addr / 4] = 0;
            return;
        }
        if (value & ESP8266_SPI_CMD_RDID) {
            s->spi_regs[ESP8266_SPI_W0 / 4] = ESP8266_FLASH_JEDEC_ID;
            s->spi_regs[addr / 4] = 0;
            return;
        }
        if (value & ESP8266_SPI_CMD_RDSR) {
            s->spi_regs[ESP8266_SPI_RD_STATUS / 4] = s->spi_flash_status1;
            s->spi_regs[ESP8266_SPI_W0 / 4] = s->spi_flash_status1;
            s->spi_regs[addr / 4] = 0;
            return;
        }
        switch (flash_op) {
        case ESP8266_SPI_FLASH_OP_WRSR:
            s->spi_flash_status1 = s->spi_regs[ESP8266_SPI_W0 / 4] & 0xff;
            s->spi_flash_status2 =
                (s->spi_regs[ESP8266_SPI_W0 / 4] >> 8) & 0xff;
            break;
        case ESP8266_SPI_FLASH_OP_WRSR2:
            s->spi_flash_status2 = s->spi_regs[ESP8266_SPI_W0 / 4] & 0xff;
            break;
        case ESP8266_SPI_FLASH_OP_RDSR1:
            s->spi_regs[ESP8266_SPI_W0 / 4] = s->spi_flash_status1;
            break;
        case ESP8266_SPI_FLASH_OP_RDSR2:
            s->spi_regs[ESP8266_SPI_W0 / 4] = ESP8266_FLASH_STATUS2_QE;
            break;
        case ESP8266_SPI_FLASH_OP_RDID:
            s->spi_regs[ESP8266_SPI_W0 / 4] = ESP8266_FLASH_JEDEC_ID;
            break;
        case ESP8266_SPI_FLASH_OP_READ:
        case ESP8266_SPI_FLASH_OP_FAST_READ:
        case ESP8266_SPI_FLASH_OP_DOUT_READ: {
            uint32_t flash_addr = s->spi_regs[ESP8266_SPI_ADDR / 4] &
                                  (ESP8266_FLASH_SIZE - 1);
            uint8_t *irom = memory_region_get_ram_ptr(&s->irom);

            for (size_t i = 0; i < 16; i++) {
                uint32_t word_addr = (flash_addr + i * sizeof(uint32_t)) &
                                     (ESP8266_FLASH_SIZE - 1);
                s->spi_regs[(ESP8266_SPI_W0 / 4) + i] =
                    ldl_le_p(irom + word_addr);
            }
            break;
        }
        case ESP8266_SPI_FLASH_OP_PP: {
            uint32_t flash_addr = s->spi_regs[ESP8266_SPI_ADDR / 4] &
                                  (ESP8266_FLASH_SIZE - 1);
            uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
            uint8_t *drom = memory_region_get_ram_ptr(&s->drom);

            if (flash_addr <= ESP8266_FLASH_SIZE - 16 * sizeof(uint32_t)) {
                for (uint32_t mirror = flash_addr;
                     mirror < ESP8266_FLASH_MAP_SIZE;
                     mirror += ESP8266_FLASH_SIZE) {
                    for (size_t i = 0; i < 16; i++) {
                        uint32_t word = s->spi_regs[(ESP8266_SPI_W0 / 4) + i];
                        stl_le_p(irom + mirror + i * sizeof(uint32_t), word);
                        stl_le_p(drom + mirror + i * sizeof(uint32_t), word);
                    }
                }
            }
            break;
        }
        case ESP8266_SPI_FLASH_OP_SE:
        case ESP8266_SPI_FLASH_OP_BE64K: {
            uint32_t flash_addr = s->spi_regs[ESP8266_SPI_ADDR / 4] &
                                  (ESP8266_FLASH_SIZE - 1);
            size_t erase_size = flash_op == ESP8266_SPI_FLASH_OP_SE ?
                                ESP8266_FLASH_SECTOR_SIZE :
                                ESP8266_FLASH_BLOCK_SIZE;
            uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
            uint8_t *drom = memory_region_get_ram_ptr(&s->drom);

            flash_addr &= ~(erase_size - 1);
            if (flash_addr <= ESP8266_FLASH_SIZE - erase_size) {
                for (uint32_t mirror = flash_addr;
                     mirror < ESP8266_FLASH_MAP_SIZE;
                     mirror += ESP8266_FLASH_SIZE) {
                    memset(irom + mirror, 0xff, erase_size);
                    memset(drom + mirror, 0xff, erase_size);
                }
            }
            break;
        }
        default:
            break;
        }
        s->spi_regs[addr / 4] = 0;
        return;
    }
    if (addr < sizeof(s->spi_regs) && (addr % 4) == 0) {
        s->spi_regs[addr / 4] = value;
    }
}

static const MemoryRegionOps esp8266_spi_ops = {
    .read = esp8266_spi_read,
    .write = esp8266_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void esp8266_flash_helper_erase(Esp8266SocState *s, uint32_t sector)
{
    uint32_t flash_addr = (sector * ESP8266_FLASH_SECTOR_SIZE) &
                          (ESP8266_FLASH_MAP_SIZE - 1);
    uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
    uint8_t *drom = memory_region_get_ram_ptr(&s->drom);

    if (flash_addr <= ESP8266_FLASH_MAP_SIZE - ESP8266_FLASH_SECTOR_SIZE) {
        memset(irom + flash_addr, 0xff, ESP8266_FLASH_SECTOR_SIZE);
        memset(drom + flash_addr, 0xff, ESP8266_FLASH_SECTOR_SIZE);
    }
}

static void esp8266_flash_helper_program(Esp8266SocState *s, uint32_t len)
{
    uint32_t flash_addr = s->flash_helper_addr & (ESP8266_FLASH_MAP_SIZE - 1);
    uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
    uint8_t *drom = memory_region_get_ram_ptr(&s->drom);
    g_autofree uint8_t *buf = NULL;

    if (len == 0 || flash_addr >= ESP8266_FLASH_MAP_SIZE) {
        return;
    }
    len = MIN(len, ESP8266_FLASH_MAP_SIZE - flash_addr);
    buf = g_malloc(len);
    address_space_read(&address_space_memory, s->flash_helper_src,
                       MEMTXATTRS_UNSPECIFIED, buf, len);
    memcpy(irom + flash_addr, buf, len);
    memcpy(drom + flash_addr, buf, len);
}

static uint64_t esp8266_flash_helper_read(void *opaque, hwaddr addr,
                                          unsigned int size)
{
    return 0;
}

static void esp8266_flash_helper_write(void *opaque, hwaddr addr,
                                       uint64_t value, unsigned int size)
{
    Esp8266SocState *s = opaque;

    switch (addr) {
    case ESP8266_FLASH_HELPER_ERASE_SECTOR:
        esp8266_flash_helper_erase(s, value);
        break;
    case ESP8266_FLASH_HELPER_WRITE_ADDR:
        s->flash_helper_addr = value;
        break;
    case ESP8266_FLASH_HELPER_WRITE_SRC:
        s->flash_helper_src = value;
        break;
    case ESP8266_FLASH_HELPER_WRITE_LEN:
        esp8266_flash_helper_program(s, value);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps esp8266_flash_helper_ops = {
    .read = esp8266_flash_helper_read,
    .write = esp8266_flash_helper_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static uint64_t esp8266_i2c_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr == ESP8266_I2C_CLOCK_GATE) {
        return s->i2c_regs[addr / 4];
    }
    if (addr < sizeof(s->i2c_regs) && (addr % 4) == 0) {
        return s->i2c_regs[addr / 4];
    }
    return 0;
}

static void esp8266_i2c_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr == ESP8266_I2C_CLOCK_GATE) {
        s->i2c_regs[addr / 4] = value;
        return;
    }
    if (addr < sizeof(s->i2c_regs) && (addr % 4) == 0) {
        s->i2c_regs[addr / 4] = value;
    }
}

static const MemoryRegionOps esp8266_i2c_ops = {
    .read = esp8266_i2c_read,
    .write = esp8266_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t esp8266_timer_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr == 0 || addr == ESP8266_TIMER_COUNT) {
        return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000;
    }
    if (addr == ESP8266_TIMER_STATUS) {
        return s->timer_regs[addr / 4] | ESP8266_TIMER_STATUS_READY;
    }
    if (addr < sizeof(s->timer_regs) && (addr % 4) == 0) {
        return s->timer_regs[addr / 4];
    }
    return 0;
}

static void esp8266_timer_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr < sizeof(s->timer_regs) && (addr % 4) == 0) {
        s->timer_regs[addr / 4] = value;
    }
}

static const MemoryRegionOps esp8266_timer_ops = {
    .read = esp8266_timer_read,
    .write = esp8266_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t esp8266_rtc_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr == ESP8266_RTC_TIMER_STATUS) {
        return s->rtc_regs[addr / 4] | ESP8266_TIMER_STATUS_READY;
    }
    if (addr < sizeof(s->rtc_regs) && (addr % 4) == 0) {
        return s->rtc_regs[addr / 4];
    }
    return 0;
}

static void esp8266_rtc_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr < sizeof(s->rtc_regs) && (addr % 4) == 0) {
        s->rtc_regs[addr / 4] = value;
    }
}

static const MemoryRegionOps esp8266_rtc_ops = {
    .read = esp8266_rtc_read,
    .write = esp8266_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t esp8266_iomux_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr < sizeof(s->iomux_regs) && (addr % 4) == 0) {
        return s->iomux_regs[addr / 4];
    }
    return 0;
}

static void esp8266_iomux_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr < sizeof(s->iomux_regs) && (addr % 4) == 0) {
        s->iomux_regs[addr / 4] = value;
    }
}

static const MemoryRegionOps esp8266_iomux_ops = {
    .read = esp8266_iomux_read,
    .write = esp8266_iomux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t esp8266_sys_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr < sizeof(s->sys_regs) && (addr % 4) == 0) {
        return s->sys_regs[addr / 4];
    }
    return 0;
}

static void esp8266_sys_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr < sizeof(s->sys_regs) && (addr % 4) == 0) {
        s->sys_regs[addr / 4] = value;
    }
}

static const MemoryRegionOps esp8266_sys_ops = {
    .read = esp8266_sys_read,
    .write = esp8266_sys_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t esp8266_wifi_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp8266SocState *s = opaque;

    switch (addr) {
    case ESP8266_WIFI_STATUS:
        return s->wifi_regs[addr / 4] | ESP8266_WIFI_STATUS_READY;
    case ESP8266_WIFI_BUSY_STATUS:
        return s->wifi_regs[addr / 4] & ~BIT(1);
    case ESP8266_WIFI_BOOT_MAGIC:
        return s->wifi_regs[addr / 4];
    default:
        if (addr < sizeof(s->wifi_regs) && (addr % 4) == 0) {
            return s->wifi_regs[addr / 4];
        }
        return 0;
    }
}

static void esp8266_wifi_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    Esp8266SocState *s = opaque;

    if (addr == ESP8266_WIFI_BOOT_MAGIC) {
        s->wifi_regs[addr / 4] = value;
        return;
    }
    if (addr < sizeof(s->wifi_regs) && (addr % 4) == 0) {
        s->wifi_regs[addr / 4] = value;
    }
}

static const MemoryRegionOps esp8266_wifi_ops = {
    .read = esp8266_wifi_read,
    .write = esp8266_wifi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void esp8266_uart_puts(Esp8266SocState *s, const char *str)
{
    if (qemu_chr_fe_backend_connected(&s->uart0_chr)) {
        qemu_chr_fe_write_all(&s->uart0_chr, (const uint8_t *)str, strlen(str));
    } else {
        qemu_log("%s", str);
    }
}

static void esp8266_apply_boot_state(Esp8266SocState *s)
{
    CPUState *cs = CPU(&s->cpu[0]);

    if (!s->boot_loaded) {
        return;
    }

    s->cpu[0].env.sregs[PS] = 0;
    s->cpu[0].env.regs[1] = ESP8266_DRAM_BASE + ESP8266_DRAM_SIZE;
    cpu_set_pc(cs, s->boot_entry);
    cs->exception_index = -1;
    xtensa_runstall(&s->cpu[0].env, false);
}

static void esp8266_patch_reset_vector(Esp8266SocState *s)
{
    uint8_t *rom = memory_region_get_ram_ptr(&s->rom);
    size_t offset = ESP8266_ROM_RESET_VECTOR - ESP8266_ROM_BASE;
    static const uint8_t reset_trampoline[] = {
        0x11, 0xfe, 0xff,       /* l32r a1, reset_vector - 8 */
        0x21, 0xfe, 0xff,       /* l32r a2, reset_vector - 4 */
        0xa0, 0x02, 0x00,       /* jx a2 */
    };

    stl_le_p(rom + offset - 8, ESP8266_DRAM_BASE + ESP8266_DRAM_SIZE);
    stl_le_p(rom + offset - 4, s->boot_entry);
    memcpy(rom + offset, reset_trampoline, sizeof(reset_trampoline));
}

static void esp8266_boot_reset(void *opaque)
{
    esp8266_apply_boot_state(opaque);
}

static void esp8266_init_dram_state(Esp8266SocState *s)
{
    uint8_t *dram = memory_region_get_ram_ptr(&s->dram);
    size_t flashchip_offset = ESP8266_ROM_FLASHCHIP - ESP8266_DRAM_BASE;
    size_t flashchip_data_offset = ESP8266_ROM_FLASHCHIP_DATA - ESP8266_DRAM_BASE;

    /*
     * The ESP8266 ROM exports flashchip at 0x3fffc714 as a RAM pointer to the
     * flash parameter block. SDK startup rewrites fields in that block after it
     * parses the image header, so seed both the pointer and sane defaults.
     */
    stl_le_p(dram + flashchip_offset, ESP8266_ROM_FLASHCHIP_DATA);
    stl_le_p(dram + flashchip_data_offset + 0x00, 0);
    stl_le_p(dram + flashchip_data_offset + 0x04, ESP8266_FLASH_SIZE);
    stl_le_p(dram + flashchip_data_offset + 0x08, ESP8266_FLASH_BLOCK_SIZE);
    stl_le_p(dram + flashchip_data_offset + 0x0c, ESP8266_FLASH_SECTOR_SIZE);
    stl_le_p(dram + flashchip_data_offset + 0x10, ESP8266_FLASH_PAGE_SIZE);
    stl_le_p(dram + flashchip_data_offset + 0x14, ESP8266_FLASH_STATUS_MASK);
    s->spi_flash_status2 = ESP8266_FLASH_STATUS2_QE;
}

static void esp8266_init_rom_stubs(Esp8266SocState *s)
{
    uint8_t *rom = memory_region_get_ram_ptr(&s->rom);
    static const uint8_t zero_result[] = {
        0x0c, 0x02,             /* movi.n a2, 0 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t ets_strlen[] = {
        0x3d, 0x02,             /* mov.n a3, a2 */
        0x42, 0x03, 0x00,       /* loop: l8ui a4, a3, 0 */
        0x8c, 0x34,             /* beqz.n a4, done */
        0x1b, 0x33,             /* addi.n a3, a3, 1 */
        0x46, 0xfd, 0xff,       /* j loop */
        0x20, 0x23, 0xc0,       /* done: sub a2, a3, a2 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t cache_read_enable[] = {
        0xa0, 0x02, 0x00,       /* jx a2 */
    };
    static const uint8_t ets_putc[] = {
        0x00, 0x00, 0x00, 0x60, /* literal: UART0 FIFO */
        0x31, 0xff, 0xff,       /* l32r a3, . - 4 */
        0xc0, 0x20, 0x00,       /* memw */
        0x29, 0x03,             /* s32i.n a2, a3, 0 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t ets_memcmp[] = {
        0x9c, 0x04,             /* beqz.n a4, equal */
        0x52, 0x02, 0x00,       /* loop: l8ui a5, a2, 0 */
        0x62, 0x03, 0x00,       /* l8ui a6, a3, 0 */
        0x67, 0x95, 0x0c,       /* bne a5, a6, diff */
        0x1b, 0x22,             /* addi.n a2, a2, 1 */
        0x1b, 0x33,             /* addi.n a3, a3, 1 */
        0x0b, 0x44,             /* addi.n a4, a4, -1 */
        0x56, 0xd4, 0xfe,       /* bnez a4, loop */
        0x0c, 0x02,             /* equal: movi.n a2, 0 */
        0x0d, 0xf0,             /* ret.n */
        0x60, 0x25, 0xc0,       /* diff: sub a2, a5, a6 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t ets_bzero[] = {
        0x4d, 0x03,             /* mov.n a4, a3 */
        0x0c, 0x03,             /* movi.n a3, 0 */
        0x5d, 0x02,             /* mov.n a5, a2 */
        0x8c, 0x84,             /* beqz.n a4, done */
        0x32, 0x45, 0x00,       /* loop: s8i a3, a5, 0 */
        0x1b, 0x55,             /* addi.n a5, a5, 1 */
        0x0b, 0x44,             /* addi.n a4, a4, -1 */
        0x56, 0x54, 0xff,       /* bnez a4, loop */
        0x0d, 0xf0,             /* done: ret.n */
    };
    static const uint8_t ets_task[] = {
        0x52, 0xa1, 0x00,       /* movi a5, 0x100 */
        0xe0, 0x63, 0x11,       /* slli a6, a3, 2 */
        0x6a, 0x55,             /* add.n a5, a5, a6 */
        0x29, 0x05,             /* s32i.n a2, a5, 0 */
        0x52, 0xa1, 0x40,       /* movi a5, 0x140 */
        0x6a, 0x55,             /* add.n a5, a5, a6 */
        0x49, 0x05,             /* s32i.n a4, a5, 0 */
        0x0c, 0x12,             /* movi.n a2, 1 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t ets_post[] = {
        0x12, 0xc1, 0xf0,       /* addi a1, a1, -16 */
        0x09, 0x31,             /* s32i.n a0, a1, 12 */
        0x52, 0xa1, 0x00,       /* movi a5, 0x100 */
        0xe0, 0x62, 0x11,       /* slli a6, a2, 2 */
        0x6a, 0x55,             /* add.n a5, a5, a6 */
        0x78, 0x05,             /* l32i.n a7, a5, 0 */
        0x9c, 0x97,             /* beqz.n a7, fail */
        0x52, 0xa1, 0x40,       /* movi a5, 0x140 */
        0x6a, 0x55,             /* add.n a5, a5, a6 */
        0x58, 0x05,             /* l32i.n a5, a5, 0 */
        0x9c, 0x05,             /* beqz.n a5, fail */
        0x39, 0x05,             /* s32i.n a3, a5, 0 */
        0x49, 0x15,             /* s32i.n a4, a5, 4 */
        0x2d, 0x05,             /* mov.n a2, a5 */
        0xc0, 0x07, 0x00,       /* callx0 a7 */
        0x0c, 0x12,             /* movi.n a2, 1 */
        0x08, 0x31,             /* l32i.n a0, a1, 12 */
        0x12, 0xc1, 0x10,       /* addi a1, a1, 16 */
        0x0d, 0xf0,             /* ret.n */
        0x0c, 0x02,             /* fail: movi.n a2, 0 */
        0x08, 0x31,             /* l32i.n a0, a1, 12 */
        0x12, 0xc1, 0x10,       /* addi a1, a1, 16 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t ets_run[] = {
        0x00, 0x70, 0x00,       /* waiti 0 */
        0x46, 0xfe, 0xff,       /* j ets_run */
    };
    static const uint8_t guarded_memset[] = {
        0x20, 0x52, 0x20,       /* or a5, a2, a2 */
        0x16, 0x94, 0x00,       /* beqz a4, done */
        0x32, 0x45, 0x00,       /* loop: s8i a3, a5, 0 */
        0x1b, 0x55,             /* addi.n a5, a5, 1 */
        0x0b, 0x44,             /* addi.n a4, a4, -1 */
        0x56, 0x54, 0xff,       /* bnez a4, loop */
        0x0d, 0xf0,             /* done: ret.n */
    };
    static const uint8_t guarded_memcpy[] = {
        0x20, 0x52, 0x20,       /* or a5, a2, a2 */
        0x16, 0xf4, 0x00,       /* beqz a4, done */
        0x62, 0x03, 0x00,       /* loop: l8ui a6, a3, 0 */
        0x62, 0x45, 0x00,       /* s8i a6, a5, 0 */
        0x1b, 0x55,             /* addi.n a5, a5, 1 */
        0x1b, 0x33,             /* addi.n a3, a3, 1 */
        0x42, 0xc4, 0xff,       /* addi a4, a4, -1 */
        0x56, 0xf4, 0xfe,       /* bnez a4, loop */
        0x0d, 0xf0,             /* done: ret.n */
    };
    static const uint8_t rtc_get_reset_reason[] = {
        0x0c, 0x02,             /* movi.n a2, power-on reset */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t spi_read[] = {
        0x00, 0x00, 0x20, 0x40, /* literal: mapped flash base */
        0x51, 0xff, 0xff,       /* l32r a5, . - 4 */
        0x57, 0x32, 0x06,       /* bltu a2, a5, add_base */
        0x5d, 0x02,             /* mov.n a5, a2 */
        0xc6, 0x00, 0x00,       /* j loop_test */
        0x00, 0x00,             /* padding */
        0x2a, 0x55,             /* add_base: add.n a5, a5, a2 */
        0x8c, 0xd4,             /* loop_test: beqz.n a4, done */
        0x62, 0x05, 0x00,       /* l8ui a6, a5, 0 */
        0x62, 0x43, 0x00,       /* s8i a6, a3, 0 */
        0x1b, 0x55,             /* addi.n a5, a5, 1 */
        0x1b, 0x33,             /* addi.n a3, a3, 1 */
        0x0b, 0x44,             /* addi.n a4, a4, -1 */
        0x56, 0x04, 0xff,       /* bnez a4, loop */
        0x0c, 0x02,             /* movi.n a2, 0 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t spi_erase_sector[] = {
        0x00, 0x14, 0x00, 0x60, /* literal: flash helper base */
        0x31, 0xff, 0xff,       /* l32r a3, . - 4 */
        0x29, 0x03,             /* s32i.n a2, a3, 0 */
        0x0c, 0x02,             /* movi.n a2, 0 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t spi_write[] = {
        0x00, 0x14, 0x00, 0x60, /* literal: flash helper base */
        0x51, 0xff, 0xff,       /* l32r a5, . - 4 */
        0x29, 0x15,             /* s32i.n a2, a5, 4 */
        0x39, 0x25,             /* s32i.n a3, a5, 8 */
        0x49, 0x35,             /* s32i.n a4, a5, 12 */
        0x0c, 0x02,             /* movi.n a2, 0 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t flash_sector_count[] = {
        0x22, 0xa1, 0x00,       /* movi a2, 0x100 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t udivdi3[] = {
        0xec, 0x45,             /* bnez.n a5, zero */
        0xac, 0x24,             /* beqz.n a4, zero */
        0x0b, 0x74,             /* addi.n a7, a4, -1 */
        0x70, 0x84, 0x10,       /* and a8, a4, a7 */
        0xdc, 0xb8,             /* bnez.n a8, zero */
        0x0c, 0x06,             /* movi.n a6, 0 */
        0x0c, 0x18,             /* movi.n a8, 1 */
        0x87, 0x14, 0x07,       /* loop: beq a4, a8, shift */
        0x40, 0x41, 0x41,       /* srli a4, a4, 1 */
        0x1b, 0x66,             /* addi.n a6, a6, 1 */
        0x06, 0xfd, 0xff,       /* j loop */
        0x8c, 0x76,             /* shift: beqz.n a6, done */
        0x00, 0x06, 0x40,       /* ssr a6 */
        0x20, 0x23, 0x81,       /* src a2, a3, a2 */
        0x30, 0x30, 0x91,       /* srl a3, a3 */
        0x0d, 0xf0,             /* done: ret.n */
        0x00,                   /* padding */
        0x0c, 0x02,             /* zero: movi.n a2, 0 */
        0x0c, 0x03,             /* movi.n a3, 0 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t umoddi3[] = {
        0x8c, 0xc4,             /* beqz.n a4, zero */
        0x0b, 0x64,             /* addi.n a6, a4, -1 */
        0x60, 0x74, 0x10,       /* and a7, a4, a6 */
        0xcc, 0x57,             /* bnez.n a7, zero */
        0x60, 0x22, 0x10,       /* and a2, a2, a6 */
        0x0c, 0x03,             /* movi.n a3, 0 */
        0x0d, 0xf0,             /* ret.n */
        0x0c, 0x02,             /* zero: movi.n a2, 0 */
        0x0c, 0x03,             /* movi.n a3, 0 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t mul_overflow_check[] = {
        0x0c, 0x03,             /* movi.n a3, 0 */
        0x0d, 0xf0,             /* ret.n */
    };

    /*
     * Early ESP8266 eboot images call a small set of ROM helpers before the
     * SDK starts. Fill the window with ret.n, then patch the helpers needed to
     * load the next flash stage.
     */
    for (size_t i = 0; i < ESP8266_ROM_SIZE; i += 2) {
        rom[i] = 0x0d;
        rom[i + 1] = 0xf0;
    }

    memcpy(rom + ESP8266_ROM_ETS_PRINTF - ESP8266_ROM_BASE,
           zero_result, sizeof(zero_result));
    memcpy(rom + ESP8266_ROM_ETS_VPRINTF - ESP8266_ROM_BASE,
           zero_result, sizeof(zero_result));
    memcpy(rom + ESP8266_ROM_ETS_STRLEN - ESP8266_ROM_BASE, ets_strlen,
           sizeof(ets_strlen));
    memcpy(rom + ESP8266_ROM_CACHE_READ_ENABLE - ESP8266_ROM_BASE,
           cache_read_enable, sizeof(cache_read_enable));
    memcpy(rom + ESP8266_ROM_ETS_PUTC - ESP8266_ROM_BASE - 4, ets_putc,
           sizeof(ets_putc));
    memcpy(rom + ESP8266_ROM_RTC_GET_RESET_REASON - ESP8266_ROM_BASE,
           rtc_get_reset_reason, sizeof(rtc_get_reset_reason));
    memcpy(rom + ESP8266_ROM_ETS_TASK - ESP8266_ROM_BASE, ets_task,
           sizeof(ets_task));
    memcpy(rom + ESP8266_ROM_ETS_RUN - ESP8266_ROM_BASE, ets_run,
           sizeof(ets_run));
    memcpy(rom + ESP8266_ROM_ETS_POST - ESP8266_ROM_BASE, ets_post,
           sizeof(ets_post));
    memcpy(rom + ESP8266_ROM_ETS_BZERO - ESP8266_ROM_BASE, ets_bzero,
           sizeof(ets_bzero));
    memcpy(rom + ESP8266_ROM_ETS_MEMSET - ESP8266_ROM_BASE,
           guarded_memset, sizeof(guarded_memset));
    memcpy(rom + ESP8266_ROM_ETS_MEMCPY - ESP8266_ROM_BASE,
           guarded_memcpy, sizeof(guarded_memcpy));
    memcpy(rom + ESP8266_ROM_ETS_MEMCMP - ESP8266_ROM_BASE,
           ets_memcmp, sizeof(ets_memcmp));
    memcpy(rom + ESP8266_ROM_SPI_READ_STATUS - ESP8266_ROM_BASE,
           zero_result, sizeof(zero_result));
    memcpy(rom + ESP8266_ROM_SPI_WRITE_STATUS - ESP8266_ROM_BASE,
           zero_result, sizeof(zero_result));
    memcpy(rom + ESP8266_ROM_SPI_WRITE_ENABLE - ESP8266_ROM_BASE,
           zero_result, sizeof(zero_result));
    memcpy(rom + ESP8266_ROM_WAIT_SPI_IDLE - ESP8266_ROM_BASE,
           zero_result, sizeof(zero_result));
    memcpy(rom + ESP8266_ROM_SPI_ERASE_CHIP - ESP8266_ROM_BASE,
           zero_result, sizeof(zero_result));
    memcpy(rom + ESP8266_ROM_SPI_ERASE_BLOCK - ESP8266_ROM_BASE,
           zero_result, sizeof(zero_result));
    memcpy(rom + ESP8266_ROM_SPI_ERASE_SECTOR - ESP8266_ROM_BASE,
           spi_erase_sector + 4, sizeof(spi_erase_sector) - 4);
    memcpy(rom + ESP8266_ROM_SPI_ERASE_SECTOR - ESP8266_ROM_BASE - 4,
           spi_erase_sector, 4);
    memcpy(rom + ESP8266_ROM_SPI_WRITE - ESP8266_ROM_BASE,
           spi_write + 4, sizeof(spi_write) - 4);
    memcpy(rom + ESP8266_ROM_SPI_WRITE - ESP8266_ROM_BASE - 4,
           spi_write, 4);
    memcpy(rom + ESP8266_ROM_SPI_READ - ESP8266_ROM_BASE - 4, spi_read,
           sizeof(spi_read));
    memcpy(rom + ESP8266_ROM_SPI_ERASE_AREA - ESP8266_ROM_BASE,
           zero_result, sizeof(zero_result));
    memcpy(rom + ESP8266_ROM_FLASH_SECTOR_COUNT - ESP8266_ROM_BASE,
           flash_sector_count, sizeof(flash_sector_count));
    memcpy(rom + ESP8266_ROM_UDIVDI3 - ESP8266_ROM_BASE,
           udivdi3, sizeof(udivdi3));
    memcpy(rom + ESP8266_ROM_UMODDI3 - ESP8266_ROM_BASE,
           umoddi3, sizeof(umoddi3));
    memcpy(rom + ESP8266_ROM_MUL_OVERFLOW_CHECK - ESP8266_ROM_BASE,
           mul_overflow_check, sizeof(mul_overflow_check));
    memcpy(rom + ESP8266_ROM_MEMCMP - ESP8266_ROM_BASE,
           ets_memcmp, sizeof(ets_memcmp));
    memcpy(rom + ESP8266_ROM_MEMCPY - ESP8266_ROM_BASE,
           guarded_memcpy, sizeof(guarded_memcpy));
    memcpy(rom + ESP8266_ROM_MEMSET - ESP8266_ROM_BASE,
           guarded_memset, sizeof(guarded_memset));
    memcpy(rom + ESP8266_ROM_STRLEN - ESP8266_ROM_BASE, ets_strlen,
           sizeof(ets_strlen));
}

static void esp8266_soc_init(Object *obj)
{
    Esp8266SocState *s = ESP8266_SOC(obj);
    object_initialize_child(obj, "cpu", &s->cpu[0], TYPE_ESP8266_CPU);
}

static void esp8266_soc_realize(DeviceState *dev, Error **errp)
{
    Esp8266SocState *s = ESP8266_SOC(dev);
    MemoryRegion *system_memory = get_system_memory();

    if (!qdev_realize(DEVICE(&s->cpu[0]), NULL, errp)) {
        return;
    }

    /*
     * Some closed ESP8266 SDK WiFi paths transiently dereference a null base
     * when the ROM/cache helpers are stubbed. Keep those writes contained so
     * they do not stop radio bring-up exploration.
     */
    memory_region_init_ram(&s->low_scratch, OBJECT(dev), "esp8266.low-scratch",
                           4 * KiB, &error_fatal);
    memory_region_add_subregion(system_memory, 0, &s->low_scratch);
    for (size_t i = 0; i < 4 * KiB; i += 4) {
        stl_le_p((uint8_t *)memory_region_get_ram_ptr(&s->low_scratch) + i,
                 0x40006b08);
    }

    /* DRAM: 0x3FFE8000 (80KB) */
    memory_region_init_ram(&s->dram, OBJECT(dev), "esp8266.dram",
                           ESP8266_DRAM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, ESP8266_DRAM_BASE, &s->dram);
    esp8266_init_dram_state(s);

    memory_region_init_ram(&s->sram, OBJECT(dev), "esp8266.sram",
                           ESP8266_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, ESP8266_SRAM_BASE, &s->sram);

    /*
     * IRAM: SDK eboot images commonly place their first segment at
     * 0x4010f000, so expose the full 64 KiB 0x40100000..0x4010ffff window.
     */
    memory_region_init_ram(&s->iram, OBJECT(dev), "esp8266.iram", 64 * KiB, &error_fatal);
    memory_region_add_subregion(system_memory, 0x40100000, &s->iram);

    memory_region_init_ram(&s->rom, OBJECT(dev), "esp8266.rom",
                           ESP8266_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, ESP8266_ROM_BASE, &s->rom);
    esp8266_init_rom_stubs(s);

    memory_region_init_io(&s->dport, OBJECT(dev), &esp8266_dport_ops, s,
                          "esp8266.dport", 0x100);
    memory_region_add_subregion(system_memory, 0x3ff00000, &s->dport);

    /* IROM (Mapped from Flash): 0x40200000 */
    memory_region_init_ram(&s->irom, OBJECT(dev), "esp8266.irom",
                           ESP8266_FLASH_MAP_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, 0x40200000, &s->irom);

    /* DROM (Mapped from Flash): 0x3f400000 */
    memory_region_init_ram(&s->drom, OBJECT(dev), "esp8266.drom",
                           ESP8266_FLASH_MAP_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, 0x3f400000, &s->drom);

    if (serial_hd(0)) {
        qemu_chr_fe_init(&s->uart0_chr, serial_hd(0), &error_abort);
    }

    memory_region_init_io(&s->uart0, OBJECT(dev), &esp8266_uart_ops, s,
                          "esp8266.uart0", 0x100);
    memory_region_add_subregion(system_memory, 0x60000000, &s->uart0);

    s->hspi_bus = ssi_create_bus(DEVICE(s), "hspi");
    memory_region_init_io(&s->hspi, OBJECT(dev), &esp8266_hspi_ops, s,
                          "esp8266.hspi", 0x100);
    memory_region_add_subregion(system_memory, 0x60000100, &s->hspi);

    memory_region_init_io(&s->gpio, OBJECT(dev), &esp8266_gpio_ops, s,
                          "esp8266.gpio", 0x100);
    memory_region_add_subregion(system_memory, 0x60000300, &s->gpio);

    memory_region_init_io(&s->spi, OBJECT(dev), &esp8266_spi_ops, s,
                          "esp8266.spi", 0x100);
    memory_region_add_subregion(system_memory, 0x60000200, &s->spi);

    memory_region_init_io(&s->flash_helper, OBJECT(dev),
                          &esp8266_flash_helper_ops, s,
                          "esp8266.flash-helper", 0x10);
    memory_region_add_subregion(system_memory, ESP8266_FLASH_HELPER_BASE,
                                &s->flash_helper);

    memory_region_init_io(&s->wdt, OBJECT(dev), &esp8266_timer_ops, s,
                          "esp8266.wdt", 0x100);
    memory_region_add_subregion(system_memory, 0x60000400, &s->wdt);

    memory_region_init_io(&s->i2c, OBJECT(dev), &esp8266_i2c_ops, s,
                          "esp8266.i2c", 0x800);
    memory_region_add_subregion(system_memory, 0x60000a00, &s->i2c);

    memory_region_init_io(&s->timer0, OBJECT(dev), &esp8266_timer_ops, s,
                          "esp8266.timer0", 0x100);
    memory_region_add_subregion(system_memory, 0x60000500, &s->timer0);

    memory_region_init_io(&s->timer, OBJECT(dev), &esp8266_timer_ops, s,
                          "esp8266.timer", 0x300);
    memory_region_add_subregion(system_memory, 0x60000600, &s->timer);

    memory_region_init_io(&s->rtc, OBJECT(dev), &esp8266_rtc_ops, s,
                          "esp8266.rtc", 0x100);
    memory_region_add_subregion(system_memory, 0x60000700, &s->rtc);

    memory_region_init_io(&s->iomux, OBJECT(dev), &esp8266_iomux_ops, s,
                          "esp8266.iomux", 0x100);
    memory_region_add_subregion(system_memory, 0x60001200, &s->iomux);

    memory_region_init_io(&s->sys, OBJECT(dev), &esp8266_sys_ops, s,
                          "esp8266.sys", 0x100);
    memory_region_add_subregion(system_memory, 0x60000900, &s->sys);

    memory_region_init_io(&s->wifi, OBJECT(dev), &esp8266_wifi_ops, s,
                          "esp8266.wifi", 0x2000);
    memory_region_add_subregion(system_memory, 0x60009000, &s->wifi);
}

static void esp8266_soc_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = esp8266_soc_realize;
}

static const TypeInfo esp8266_soc_type_info = {
    .name = TYPE_ESP8266_SOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp8266SocState),
    .instance_init = esp8266_soc_init,
    .class_init = esp8266_soc_class_init,
};

static void esp8266_load_raw_flash(Esp8266SocState *s, const uint8_t *data,
                                   size_t len)
{
    uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
    uint8_t *drom = memory_region_get_ram_ptr(&s->drom);
    size_t copy_len = MIN(len, (size_t)ESP8266_FLASH_SIZE);

    for (uint32_t mirror = 0; mirror < ESP8266_FLASH_MAP_SIZE;
         mirror += ESP8266_FLASH_SIZE) {
        memcpy(irom + mirror, data, copy_len);
        memcpy(drom + mirror, data, copy_len);
        if (copy_len < ESP8266_FLASH_SIZE) {
            memset(irom + mirror + copy_len, 0xff,
                   ESP8266_FLASH_SIZE - copy_len);
            memset(drom + mirror + copy_len, 0xff,
                   ESP8266_FLASH_SIZE - copy_len);
        }
    }
}

static bool esp8266_load_image_segments(const uint8_t *data, size_t len,
                                        uint32_t *entry)
{
    uint8_t segments;
    size_t offset = 8;

    if (len < 8 || data[0] != ESP8266_IMAGE_MAGIC) {
        return false;
    }

    segments = data[1];
    *entry = ldl_le_p(data + 4);

    for (uint8_t i = 0; i < segments; i++) {
        uint32_t load_addr;
        uint32_t seg_len;

        if (offset + 8 > len) {
            warn_report("ESP8266 image truncated before segment %u header", i);
            return false;
        }

        load_addr = ldl_le_p(data + offset);
        seg_len = ldl_le_p(data + offset + 4);
        offset += 8;

        if (seg_len > len - offset) {
            warn_report("ESP8266 image segment %u truncated: addr=0x%08x len=%u",
                        i, load_addr, seg_len);
            return false;
        }

        cpu_physical_memory_write(load_addr, data + offset, seg_len);
        qemu_log("ESP8266 image segment %u: load=0x%08x size=%u\n",
                 i, load_addr, seg_len);
        offset += seg_len;
    }

    return true;
}

static bool esp8266_patch_irom_helper(Esp8266SocState *s, uint32_t addr,
                                      const uint8_t *prologue,
                                      size_t prologue_size)
{
    uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
    size_t offset = addr - ESP8266_FLASH_BASE;
    static const uint8_t zero_result[] = {
        0x0c, 0x02,             /* movi.n a2, 0 */
        0x0d, 0xf0,             /* ret.n */
    };

    if (offset + prologue_size > ESP8266_FLASH_MAP_SIZE ||
        memcmp(irom + offset, prologue, prologue_size) != 0) {
        return false;
    }

    memcpy(irom + offset, zero_result, sizeof(zero_result));
    return true;
}

static void esp8266_patch_sdk_flash_helpers(Esp8266SocState *s)
{
    static const uint8_t sdk_flash_probe_prologue[] = {
        0x7d, 0x04,             /* mov.n a7, a4 */
        0x12, 0xc1, 0xe0,       /* addi a1, a1, -32 */
    };
    static const uint8_t sdk_flash_erase_range_prologue[] = {
        0x12, 0xc1, 0xf0,       /* addi a1, a1, -16 */
        0x20, 0x43, 0x20,       /* or a4, a3, a2 */
    };

    /*
     * The checked ESP8266 SDK image reaches this helper during flash-size
     * probing. It destructively erases and rewrites many sectors before ELRS
     * setup can run. QEMU already models the ROM flash APIs it calls; skip the
     * probe itself so radio bring-up is not gated on a slow flash self-test.
     */
    if (esp8266_patch_irom_helper(s, ESP8266_SDK_FLASH_PROBE,
                                  sdk_flash_probe_prologue,
                                  sizeof(sdk_flash_probe_prologue))) {
        qemu_log("ESP8266 patched SDK flash probe at 0x%08x\n",
                 ESP8266_SDK_FLASH_PROBE);
    }

    /*
     * SPIFFS/EEPROM initialization can format its flash region sector by
     * sector on an empty image. Skip the erase-range wrapper; persistence is
     * out of scope for the current radio bring-up target.
     */
    if (esp8266_patch_irom_helper(s, ESP8266_SDK_FLASH_ERASE_RANGE,
                                  sdk_flash_erase_range_prologue,
                                  sizeof(sdk_flash_erase_range_prologue))) {
        qemu_log("ESP8266 patched SDK flash erase range at 0x%08x\n",
                 ESP8266_SDK_FLASH_ERASE_RANGE);
    }
}

static void esp8266_patch_sdk_spi_read_wrapper(Esp8266SocState *s)
{
    uint8_t *iram = memory_region_get_ram_ptr(&s->iram);
    size_t offset = ESP8266_SDK_SPI_READ_WRAPPER - 0x40100000;
    static const uint8_t spi_read_wrapper_prologue[] = {
        0x12, 0xc1, 0xf0,       /* addi a1, a1, -16 */
        0x09, 0x01,             /* s32i.n a0, a1, 0 */
    };
    static const uint8_t spi_read_tailcall[] = {
        0x1c, 0x4b, 0x00, 0x40, /* literal: ROM SPIRead */
        0x51, 0xff, 0xff,       /* l32r a5, . - 4 */
        0xa0, 0x05, 0x00,       /* jx a5 */
    };

    /*
     * The SDK wrapper around SPIRead disables/enables cache around every tiny
     * flash read. The flash is RAM-backed in this machine and the ROM SPIRead
     * stub already copies from mapped flash, so tail-call it directly.
     */
    if (offset >= 4 &&
        offset + sizeof(spi_read_wrapper_prologue) <= 64 * KiB &&
        memcmp(iram + offset, spi_read_wrapper_prologue,
               sizeof(spi_read_wrapper_prologue)) == 0) {
        memcpy(iram + offset - 4, spi_read_tailcall, sizeof(spi_read_tailcall));
        qemu_log("ESP8266 patched SDK SPIRead wrapper at 0x%08x\n",
                 ESP8266_SDK_SPI_READ_WRAPPER);
    }
}

static void esp8266_patch_sdk_spiffs_open(Esp8266SocState *s)
{
    uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
    uint8_t *drom = memory_region_get_ram_ptr(&s->drom);
    size_t offset = ESP8266_SDK_SPIFFS_OPEN - ESP8266_FLASH_BASE;
    static const uint8_t spiffs_open_prologue[] = {
        0x12, 0xc1, 0xb0,       /* addi a1, a1, -80 */
        0xe2, 0x61, 0x10,       /* s32i a14, a1, 64 */
    };
    static const uint8_t force_no_impl[] = {
        0x0c, 0x04,             /* movi.n a4, 0 */
    };

    /*
     * The ELRS image carries options/hardware JSON in its firmware trailer.
     * Until this machine models SPIFFS, make SPIFFS.open() return an empty
     * File so options_init() uses the trailer and can continue to radio setup.
     */
    if (offset + sizeof(spiffs_open_prologue) <= ESP8266_FLASH_MAP_SIZE &&
        memcmp(irom + offset, spiffs_open_prologue,
               sizeof(spiffs_open_prologue)) == 0) {
        memcpy(irom + offset + 8, force_no_impl, sizeof(force_no_impl));
        memcpy(drom + offset + 8, force_no_impl, sizeof(force_no_impl));
        qemu_log("ESP8266 patched SDK SPIFFS open at 0x%08x\n",
                 ESP8266_SDK_SPIFFS_OPEN);
    }
}

static void esp8266_patch_sdk_hardware_json_reserve(Esp8266SocState *s)
{
    uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
    uint8_t *drom = memory_region_get_ram_ptr(&s->drom);
    size_t offset = ESP8266_SDK_HARDWARE_JSON_RESERVE - ESP8266_FLASH_BASE;
    static const uint8_t reserve_size[] = {
        0x22, 0xd2, 0x08,       /* addmi a2, a2, 0x800 */
    };
    static const uint8_t nop[] = {
        0xf0, 0x20, 0x00,       /* nop */
    };

    /*
     * Skip the 2 KiB Web UI hardware JSON mirror reserve after fields are
     * parsed from the firmware trailer. The radio setup only needs the loaded
     * hardware table, not the serialized mirror string.
     */
    if (offset + sizeof(reserve_size) <= ESP8266_FLASH_MAP_SIZE &&
        memcmp(irom + offset, reserve_size, sizeof(reserve_size)) == 0) {
        memcpy(irom + offset, nop, sizeof(nop));
        memcpy(drom + offset, nop, sizeof(nop));
        qemu_log("ESP8266 patched SDK hardware JSON reserve at 0x%08x\n",
                 ESP8266_SDK_HARDWARE_JSON_RESERVE);
    }
}

static void esp8266_patch_sdk_rx_uid_log(Esp8266SocState *s)
{
    uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
    uint8_t *drom = memory_region_get_ram_ptr(&s->drom);
    size_t offset = ESP8266_SDK_RX_UID_LOG_CALL - ESP8266_FLASH_BASE;
    const uint32_t string_calls[] = {
        ESP8266_SDK_RX_UID_LOG_STRING1,
        ESP8266_SDK_RX_UID_LOG_STRING2,
        ESP8266_SDK_RX_UID_LOG_STRING3,
    };
    static const uint8_t uid_log_call[] = {
        0x05, 0x20, 0xff,       /* call0 0x40207b38 */
    };
    static const uint8_t nop[] = {
        0xf0, 0x20, 0x00,       /* nop */
    };

    /*
     * The current ELRS RX image reaches radio setup through a DEBUG_LOG-style
     * UID print. In this ROM-stubbed ESP8266 environment that String/printf
     * path can dominate execution before any radio SPI is issued; skip only
     * this diagnostic call and preserve the surrounding setup logic.
     */
    if (offset + sizeof(uid_log_call) <= ESP8266_FLASH_MAP_SIZE &&
        memcmp(irom + offset, uid_log_call, sizeof(uid_log_call)) == 0) {
        memcpy(irom + offset, nop, sizeof(nop));
        memcpy(drom + offset, nop, sizeof(nop));
        qemu_log("ESP8266 patched SDK RX UID log at 0x%08x\n",
                 ESP8266_SDK_RX_UID_LOG_CALL);
    }

    /*
     * The same debug-only block immediately performs several String/print
     * cleanup calls before returning to setup. Keep the stack frame and local
     * moves intact, but skip the call sites so radio bring-up can proceed.
     */
    for (size_t i = 0; i < ARRAY_SIZE(string_calls); i++) {
        offset = string_calls[i] - ESP8266_FLASH_BASE;
        if (offset + sizeof(nop) <= ESP8266_FLASH_MAP_SIZE) {
            memcpy(irom + offset, nop, sizeof(nop));
            memcpy(drom + offset, nop, sizeof(nop));
            qemu_log("ESP8266 patched SDK RX UID log string call at 0x%08x\n",
                     string_calls[i]);
        }
    }
}

static void esp8266_seed_elrs_hardware_table(void)
{
    uint32_t hardware[ESP8266_ELRS_HARDWARE_FIELDS];

    for (size_t i = 0; i < ARRAY_SIZE(hardware); i++) {
        hardware[i] = UINT32_MAX;
    }

    /* Generic ESP8285 SX127x 900MHz RX hardware trailer values. */
    hardware[0] = 3;            /* serial_rx */
    hardware[1] = 1;            /* serial_tx */
    hardware[6] = 4;            /* radio_dio0 */
    hardware[8] = 5;            /* radio_dio1 */
    hardware[10] = 12;          /* radio_miso */
    hardware[11] = 13;          /* radio_mosi */
    hardware[12] = 15;          /* radio_nss */
    hardware[14] = 2;           /* radio_rst */
    hardware[16] = 14;          /* radio_sck */
    hardware[17] = 0;           /* radio_dcdc */
    hardware[18] = 0;           /* radio_rfo_hf */
    hardware[19] = 0;           /* radio_rfsw_ctrl */
    hardware[20] = 0;           /* radio_rfsw_ctrl_count */
    hardware[30] = 0;           /* power_min */
    hardware[31] = 2;           /* power_high */
    hardware[32] = 2;           /* power_max */
    hardware[33] = 2;           /* power_default */
    hardware[37] = 0;           /* power_control */
    hardware[38] = 0;           /* power_values */
    hardware[39] = 0;           /* power_values_count */
    hardware[48] = 0;           /* button */
    hardware[52] = 16;          /* led */

    cpu_physical_memory_write(ESP8266_ELRS_HARDWARE_TABLE, hardware,
                              sizeof(hardware));
    qemu_log("ESP8266 seeded ELRS hardware table at 0x%08x\n",
             ESP8266_ELRS_HARDWARE_TABLE);
}

static void esp8266_patch_sdk_hardware_init(Esp8266SocState *s)
{
    uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
    uint8_t *drom = memory_region_get_ram_ptr(&s->drom);
    size_t offset = ESP8266_SDK_HARDWARE_INIT - ESP8266_FLASH_BASE;
    static const uint8_t hardware_init_prologue[] = {
        0x92, 0xa1, 0xa0,       /* movi a9, 0x1a0 */
        0x90, 0x11, 0xc0,       /* sub a1, a1, a9 */
    };
    static const uint8_t true_result[] = {
        0x0c, 0x12,             /* movi.n a2, 1 */
        0x0d, 0xf0,             /* ret.n */
    };

    if (offset + sizeof(hardware_init_prologue) <= ESP8266_FLASH_MAP_SIZE &&
        memcmp(irom + offset, hardware_init_prologue,
               sizeof(hardware_init_prologue)) == 0) {
        memcpy(irom + offset, true_result, sizeof(true_result));
        memcpy(drom + offset, true_result, sizeof(true_result));
        esp8266_seed_elrs_hardware_table();
        qemu_log("ESP8266 patched SDK hardware init at 0x%08x\n",
                 ESP8266_SDK_HARDWARE_INIT);
    }
}

static void esp8266_disable_elrs_wifi_device(void)
{
    uint32_t disabled_device[] = {
        0,                      /* initialize */
        0,                      /* start */
        0,                      /* event */
        0,                      /* timeout */
        0,                      /* subscribe */
    };

    /*
     * The current RX image initializes the WiFi device before radio setup.
     * That enters the closed ESP8266 WiFi SDK and schedules background STA
     * work that dominates execution in this minimal machine. Disable only
     * the ELRS WIFI_device hooks so setup can continue to SX127x bring-up.
     */
    cpu_physical_memory_write(ESP8266_ELRS_WIFI_DEVICE, disabled_device,
                              sizeof(disabled_device));
    qemu_log("ESP8266 disabled ELRS WIFI_device at 0x%08x\n",
             ESP8266_ELRS_WIFI_DEVICE);
}

static void esp8266_patch_elrs_devices(Esp8266SocState *s)
{
    uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
    uint8_t *drom = memory_region_get_ram_ptr(&s->drom);
    size_t real_init_offset =
        ESP8266_ELRS_DEVICES_INIT_REAL - ESP8266_FLASH_BASE;
    static const uint8_t real_devices_init_prologue[] = {
        0x12, 0xc1, 0xf0,       /* addi a1, a1, -16 */
        0xd9, 0x11,             /* s32i.n a13, a1, 4 */
    };
    static const uint8_t ret[] = {
        0x0d, 0xf0,             /* ret.n */
    };

    /*
     * ELRS UI devices initialize before radio setup and can enter SDK WiFi,
     * web, UART, and peripheral paths that are unrelated to SX127x bring-up.
     * Skip device init/start for this closed RX image while preserving the
     * radio setup path that follows.
     */
    if (real_init_offset + sizeof(real_devices_init_prologue) <=
        ESP8266_FLASH_MAP_SIZE &&
        memcmp(irom + real_init_offset, real_devices_init_prologue,
               sizeof(real_devices_init_prologue)) == 0) {
        memcpy(irom + real_init_offset, ret, sizeof(ret));
        memcpy(drom + real_init_offset, ret, sizeof(ret));
        qemu_log("ESP8266 patched ELRS devicesInit at 0x%08x\n",
                 ESP8266_ELRS_DEVICES_INIT_REAL);
    }
}

static void esp8266_seed_elrs_fhss_state(void)
{
    uint32_t fhss_config = ESP8266_ELRS_FHSS_DOMAIN_FCC900;
    uint32_t freq_spread = 0x266662;
    uint32_t sync_channel = 20;
    uint16_t primary_band_count = 240;
    uint8_t sequence[256];

    for (size_t i = 0; i < ARRAY_SIZE(sequence); i++) {
        sequence[i] = i % 40;
    }
    sequence[0] = sync_channel;

    cpu_physical_memory_write(ESP8266_ELRS_FHSS_CONFIG_PTR, &fhss_config,
                              sizeof(fhss_config));
    cpu_physical_memory_write(ESP8266_ELRS_FHSS_FREQ_SPREAD, &freq_spread,
                              sizeof(freq_spread));
    cpu_physical_memory_write(ESP8266_ELRS_FHSS_SYNC_CHANNEL, &sync_channel,
                              sizeof(sync_channel));
    cpu_physical_memory_write(ESP8266_ELRS_FHSS_PRIMARY_BAND_COUNT,
                              &primary_band_count,
                              sizeof(primary_band_count));
    cpu_physical_memory_write(ESP8266_ELRS_FHSS_SEQUENCE, sequence,
                              sizeof(sequence));
    qemu_log("ESP8266 seeded ELRS FHSS state at 0x%08x\n",
             ESP8266_ELRS_FHSS_CONFIG_PTR);
}

static void esp8266_patch_elrs_fhss_randomise(Esp8266SocState *s)
{
    uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
    uint8_t *drom = memory_region_get_ram_ptr(&s->drom);
    size_t offset = ESP8266_ELRS_FHSS_RANDOMISE - ESP8266_FLASH_BASE;
    static const uint8_t fhss_randomise_prologue[] = {
        0x12, 0xc1, 0xc0,       /* addi a1, a1, -64 */
        0xd9, 0xd1,             /* s32i.n a13, a1, 52 */
    };
    static const uint8_t ret[] = {
        0x0d, 0xf0,             /* ret.n */
    };

    if (offset + sizeof(fhss_randomise_prologue) <= ESP8266_FLASH_MAP_SIZE &&
        memcmp(irom + offset, fhss_randomise_prologue,
               sizeof(fhss_randomise_prologue)) == 0) {
        memcpy(irom + offset, ret, sizeof(ret));
        memcpy(drom + offset, ret, sizeof(ret));
        esp8266_seed_elrs_fhss_state();
        qemu_log("ESP8266 patched ELRS FHSS randomise at 0x%08x\n",
                 ESP8266_ELRS_FHSS_RANDOMISE);
    }
}

static void esp8266_patch_elrs_sx127x_hal(Esp8266SocState *s)
{
    uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
    uint8_t *drom = memory_region_get_ram_ptr(&s->drom);
    size_t offset = ESP8266_ELRS_SX127X_HAL_RESET_REAL - ESP8266_FLASH_BASE;
    static const uint8_t reset_prologue[] = {
        0x12, 0xc1, 0xf0,       /* addi a1, a1, -16 */
    };
    static const uint8_t ret[] = {
        0x0d, 0xf0,             /* ret.n */
    };

    /*
     * The reset pin dance and its delay are not observable by the QEMU SX127x
     * device. Skip it so radio bring-up reaches DetectChip/SPI transfers.
     */
    if (offset + sizeof(reset_prologue) <= ESP8266_FLASH_MAP_SIZE &&
        memcmp(irom + offset, reset_prologue, sizeof(reset_prologue)) == 0) {
        memcpy(irom + offset, ret, sizeof(ret));
        memcpy(drom + offset, ret, sizeof(ret));
        qemu_log("ESP8266 patched ELRS SX127xHal::reset at 0x%08x\n",
                 ESP8266_ELRS_SX127X_HAL_RESET_REAL);
    }
}

static void esp8266_load_flash_image(Esp8266SocState *s, const uint8_t *data,
                                     size_t len, const char *name)
{
    uint32_t entry = ESP8266_FLASH_BASE;
    const uint8_t *image_data = data;
    size_t image_len = len;
    const char *image_kind = "image";

    esp8266_load_raw_flash(s, data, len);

    /*
     * ESP8266 flash dumps commonly contain a small eboot/RAM image at 0x0 and
     * the application image at 0x1000. Load the application directly so QEMU
     * does not need to model the full SDK flash-maintenance boot path before
     * reaching user firmware.
     */
    if (len > 0x1008 && data[0x1000] == ESP8266_IMAGE_MAGIC) {
        image_data = data + 0x1000;
        image_len = len - 0x1000;
        image_kind = "app@0x1000";
    }

    if (esp8266_load_image_segments(image_data, image_len, &entry)) {
        qemu_log("ESP8266 %s %s entry=0x%08x\n", image_kind, name, entry);
        esp8266_patch_sdk_flash_helpers(s);
        esp8266_patch_sdk_spi_read_wrapper(s);
        esp8266_patch_sdk_spiffs_open(s);
        esp8266_patch_sdk_hardware_init(s);
        esp8266_patch_sdk_hardware_json_reserve(s);
        esp8266_patch_sdk_rx_uid_log(s);
        esp8266_disable_elrs_wifi_device();
        esp8266_patch_elrs_devices(s);
        esp8266_patch_elrs_fhss_randomise(s);
        esp8266_patch_elrs_sx127x_hal(s);
    } else {
        warn_report("ESP8266 image %s is not a parseable ESP8266 image; "
                    "jumping to 0x%08x", name, entry);
    }

    esp8266_uart_puts(s, "rst:0x1 (POWERON_RESET),boot:0x0 (qemu)\r\n");
    esp8266_uart_puts(s, "eboot: qemu minimal ESP8266 image loader\r\n");
    s->boot_entry = entry;
    s->boot_loaded = true;
    esp8266_patch_reset_vector(s);
    esp8266_apply_boot_state(s);
}

static void esp8266_machine_init_radios(Esp8266SocState *ss,
                                        const EspRadioBoardConfig *cfg,
                                        const char *air_chardev_name)
{
    sx127x_linker_anchor();

    if (!cfg || cfg->type == ESP_RADIO_NONE) {
        return;
    }

    if (cfg->type != ESP_RADIO_SX127X || cfg->spi_bus != 2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ESP8266: unsupported radio config type=%s spi=%d; "
                      "only sx127x on HSPI/radio_spi=2 is wired\n",
                      esp_radio_type_str(cfg->type), cfg->spi_bus);
        return;
    }

    Chardev *air_chr = NULL;
    if (air_chardev_name) {
        air_chr = qemu_chr_find(air_chardev_name);
        if (!air_chr) {
            error_report("Error: chardev '%s' not found for radio-air-chardev",
                         air_chardev_name);
        }
    }

    DeviceState *radio = qdev_new(TYPE_SX127X);
    object_property_add_child(OBJECT(ss), "radio-hspi-cs0", OBJECT(radio));
    qdev_prop_set_uint8(radio, "spi_id", 2);
    qdev_prop_set_uint8(radio, "cs", 0);
    if (air_chr) {
        qdev_prop_set_chr(radio, "air-chardev", air_chr);
    }
    qdev_realize_and_unref(radio, BUS(ss->hspi_bus), &error_fatal);
    ss->hspi_cs = qdev_get_gpio_in_named(radio, SSI_GPIO_CS, 0);

    qemu_set_irq(ss->hspi_cs, 1);
    qemu_log("ESP8266 radio board: type=sx127x spi=2 nss=%d dio0=%d dio1=%d\n",
             cfg->chips[0].nss, cfg->chips[0].dio0, cfg->chips[0].dio1);
}

static void esp8266_machine_init(MachineState *machine)
{
    Esp8266MachineState *ms = ESP8266_MACHINE(machine);
    DeviceState *soc = qdev_new(TYPE_ESP8266_SOC);
    Esp8266SocState *ss;
    EspRadioBoardConfig radio_cfg;
    bool have_radio_cfg = false;

    esp_radio_config_log("ESP8266", ms->radio_config);
    if (ms->radio_config) {
        Error *err = NULL;

        have_radio_cfg = esp_radio_board_config_load(ms->radio_config,
                                                     &radio_cfg, &err);
        if (!have_radio_cfg) {
            error_report_err(err);
        }
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(soc), &error_fatal);
    ss = ESP8266_SOC(soc);
    qemu_register_reset(esp8266_boot_reset, ss);
    cpu_reset(CPU(&ss->cpu[0]));
    esp8266_machine_init_radios(ss, have_radio_cfg ? &radio_cfg : NULL,
                                ms->radio_air_chardev);

    if (machine->kernel_filename) {
        gsize len = 0;
        g_autofree uint8_t *data = NULL;
        GError *gerr = NULL;

        if (!g_file_get_contents(machine->kernel_filename, (gchar **)&data,
                                 &len, &gerr)) {
            error_report("Could not load ESP8266 image %s: %s",
                         machine->kernel_filename, gerr->message);
            g_error_free(gerr);
            exit(1);
        }
        esp8266_load_flash_image(ss, data, len, machine->kernel_filename);
    }

    /* If flash is provided as -drive file=..., load and parse the image. */
    DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        BlockBackend *blk = blk_by_legacy_dinfo(dinfo);
        int64_t len = blk_getlength(blk);
        /*
         * This first-cut machine consumes the raw flash into RAM directly
         * instead of attaching a full SPI flash device. Mark the legacy drive
         * claimed so vl.c does not reject it as an orphaned if=mtd drive.
         */
        dinfo->is_default = true;
        if (len > 0 && !machine->kernel_filename) {
            g_autofree uint8_t *data = g_malloc(len);
            int ret = blk_pread(blk, 0, len, data, 0);
            if (ret < 0) {
                error_report("Could not read ESP8266 MTD image: %d", ret);
                exit(1);
            }
            esp8266_load_flash_image(ss, data, len, "mtd0");
        }
    }
}

ESP_RADIO_OPTIONS_DEFINE_ACCESSORS(esp8266_machine, Esp8266MachineState,
                                   ESP8266_MACHINE)

static void esp8266_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "ESP8266 (LX106)";
    mc->init = esp8266_machine_init;
    mc->default_cpu_type = TYPE_ESP8266_CPU;
    mc->block_default_type = IF_MTD;
    mc->units_per_default_bus = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
    ESP_RADIO_OPTIONS_ADD_PROPS(oc, esp8266_machine);
}

static const TypeInfo esp8266_machine_type_info = {
    .name = TYPE_ESP8266_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Esp8266MachineState),
    .class_init = esp8266_machine_class_init,
};

static void esp8266_register_types(void)
{
    type_register_static(&esp8266_soc_type_info);
    type_register_static(&esp8266_machine_type_info);
}

type_init(esp8266_register_types)
