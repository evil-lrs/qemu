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
#include "hw/ssi/sx128x.h"
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
#define ESP8266_ROM_SIZE (512 * KiB)
#define ESP8266_ROM_RESET_VECTOR 0x40000080
#define ESP8266_ROM_NOOP_RET ESP8266_ROM_BASE
#define ESP8266_ROM_KERNEL_EXCEPTION_VECTOR 0x40000030
#define ESP8266_ROM_USER_EXCEPTION_VECTOR 0x40000050
#define ESP8266_ROM_XTOS_SET_EXCEPTION_HANDLER 0x40000454
#define ESP8266_ROM_XTOS_L1INT_HANDLER 0x4000048c
#define ESP8266_ROM_CACHE_READ_ENABLE 0x4000242c
#define ESP8266_ROM_ETS_VPRINTF 0x40001f00
#define ESP8266_ROM_ETS_MEMSET 0x400018a4
#define ESP8266_ROM_ETS_MEMCPY 0x400018b4
#define ESP8266_ROM_ETS_MEMMOVE 0x400018c4
#define ESP8266_ROM_ETS_MEMCMP 0x400018d4
#define ESP8266_ROM_ETS_TASK 0x40000dd0
#define ESP8266_ROM_ETS_RUN 0x40000e04
#define ESP8266_ROM_ETS_POST 0x40000e24
#define ESP8266_ROM_ETS_ISR_ATTACH 0x40000f88
#define ESP8266_ROM_ETS_ISR_MASK 0x40000f98
#define ESP8266_ROM_ETS_ISR_UNMASK 0x40000fa8
#define ESP8266_ROM_ETS_ISR_ATTACH_IMPL 0x40001000
#define ESP8266_ROM_ETS_ISR_MASK_IMPL 0x40001020
#define ESP8266_ROM_ETS_ISR_UNMASK_IMPL 0x40001040
#define ESP8266_ROM_UDIVSI3_ALIAS 0x40001058
#define ESP8266_ROM_ETS_PRINTF 0x400024cc
#define ESP8266_ROM_ETS_PUTC 0x40002be8
#define ESP8266_ROM_ETS_BZERO 0x40002ae8
#define ESP8266_ROM_ETS_UPDATE_CPU_FREQUENCY 0x40002f04
#define ESP8266_ROM_ETS_GET_CPU_FREQUENCY 0x40002f0c
#define ESP8266_ROM_ETS_STRCPY 0x40002a88
#define ESP8266_ROM_ETS_STRNCPY 0x40002a98
#define ESP8266_ROM_ETS_STRNCMP 0x40002ab8
#define ESP8266_ROM_ETS_STRLEN 0x40002ac8
#define ESP8266_ROM_RTC_GET_RESET_REASON 0x400025e0
#define ESP8266_ROM_PHY_GET_ROMFUNCS 0x40006b08
#define ESP8266_ROM_I2C_READREG 0x40007268
#define ESP8266_ROM_I2C_READREG_MASK 0x4000729c
#define ESP8266_ROM_I2C_WRITEREG 0x400072d8
#define ESP8266_ROM_I2C_WRITEREG_MASK 0x4000730c
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
#define ESP8266_ROM_EXCEPTION_TABLE 0x3fffc000
#define ESP8266_LEVEL1_INTERRUPT_CAUSE 4
#define ESP8266_ROM_DIVSI3 0x4000dc88
#define ESP8266_ROM_UMULSIDI3 0x4000dcf0
#define ESP8266_ROM_STRCMP 0x4000bdc8
#define ESP8266_ROM_STRNCMP 0x4000bfa8
#define ESP8266_ROM_MEMCMP 0x4000dea8
#define ESP8266_ROM_MEMCPY 0x4000df48
#define ESP8266_ROM_MEMMOVE 0x4000e04c
#define ESP8266_ROM_MEMSET 0x4000e190
#define ESP8266_ROM_STRLEN 0x4000bf4c
#define ESP8266_ROM_UDIVSI3 0x4000e21c
#define ESP8266_ROM_UMODSI3 0x4000e268
#define ESP8266_ROM_COMPAT_RET 0x40036661
#define ESP8266_ROM_FLASHCHIP 0x3fffc714
#define ESP8266_ROM_FLASHCHIP_DATA 0x3fffc718
#define ESP8266_SDK_TIME_COUNTER 0x3ff20c00
#define ESP8266_SDK_TIME_COUNTER_MS (ESP8266_SDK_TIME_COUNTER + 4)
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
#define ESP8266_FLASH_HELPER_READ_ADDR 0x10
#define ESP8266_FLASH_HELPER_READ_DST 0x14
#define ESP8266_FLASH_HELPER_READ_LEN 0x18
#define ESP8266_TRACE_FLASH_HELPER 0
#define ESP8266_TRACE_PERIPH 0
#define ESP8266_ETS_SCRATCH_BASE 0x60010000
#define ESP8266_ETS_SCRATCH_SIZE (1 * MiB)
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
#define ESP8266_GPIO_OUT 0x00
#define ESP8266_GPIO_OUT_W1TS 0x04
#define ESP8266_GPIO_OUT_W1TC 0x08
#define ESP8266_GPIO_ENABLE 0x0c
#define ESP8266_GPIO_ENABLE_W1TS 0x24
#define ESP8266_GPIO_ENABLE_W1TC 0x28
#define ESP8266_GPIO_IN 0x18
#define ESP8266_GPIO_IDLE_INPUTS 0x0001ffffu
#define ESP8266_GPIO_BOOT_STRAPS ((1u << 0) | (1u << 2))
#define ESP8266_I2C_CLOCK_GATE 0x348
#define ESP8266_I2C_PHY_CTRL 0x34c
#define ESP8266_I2C_PHY_RESULT (BIT(31) | BIT(30) | BIT(24))
#define ESP8266_WIFI_BOOT_MAGIC 0x0d74
#define ESP8266_WIFI_STATUS 0x0800
#define ESP8266_WIFI_STATUS_READY (10u << 16)
#define ESP8266_WIFI_BUSY_STATUS 0x0b60
#define ESP8266_TIMER_STATUS 0x128
#define ESP8266_TIMER_STATUS_READY 0x1
#define ESP8266_TIMER_FRC1_LOAD 0x00
#define ESP8266_TIMER_FRC1_CTRL 0x08
#define ESP8266_TIMER_FRC1_INT 0x0c
#define ESP8266_TIMER_FRC1_CTRL_INT_STATUS BIT(8)
#define ESP8266_TIMER_FRC1_CTRL_ENABLE BIT(7)
#define ESP8266_TIMER_FRC1_CTRL_AUTORELOAD BIT(6)
#define ESP8266_TIMER_FRC1_LOAD_MASK 0x007fffff
#define ESP8266_TIMER_COUNT 0x04
#define ESP8266_TIMER_FRC1_COUNT 0x24
#define ESP8266_RTC_TIMER_STATUS (ESP8266_TIMER_STATUS - 0x100)
#define ESP8266_TIME_STEP_US 1000
#define ESP8266_FRC1_TICK_NS SCALE_MS
#define ESP8266_ETS_TASKS_OFF 0x00
#define ESP8266_ETS_QUEUES_OFF 0x80
#define ESP8266_ETS_PENDING_OFF 0x100
#define ESP8266_ETS_QLEN_OFF 0x180
#define ESP8266_ETS_QHEAD_OFF 0x200
#define ESP8266_ETS_QTAIL_OFF 0x280
#define ESP8266_ETS_TIMER_PRIORITY 31
#define ESP8266_ETS_LOOP_PRIORITY 1
#define ESP8266_ETS_PUMP_TICK_NS SCALE_MS
#define ESP8266_LOW_SCRATCH_BASE 0x0
#define ESP8266_LOW_SCRATCH_SIZE (2 * MiB)
#define ESP8266_PHY_ROMFUNCS_BASE (ESP8266_LOW_SCRATCH_BASE + 0x100)
#define ESP8266_PHY_ROMFUNC_NOOP_0 0
#define ESP8266_PHY_ROMFUNC_I2C_WRITEREG 152
#define ESP8266_PHY_ROMFUNC_I2C_WRITEREG_MASK_A 156
#define ESP8266_PHY_ROMFUNC_I2C_READREG 168
#define ESP8266_PHY_ROMFUNC_I2C_WRITEREG_MASK 172
#define ESP8266_PHY_ROMFUNC_NOOP_188 188
#define ESP8266_PHY_ROMFUNC_NOOP_200 200
#define ESP8266_PHY_ROMFUNCS_SIZE 0x300

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

static void esp8266_frc1_timer_cb(void *opaque)
{
    Esp8266SocState *s = opaque;
    uint32_t ctrl = s->timer_regs[ESP8266_TIMER_FRC1_CTRL / 4];

    if (!(ctrl & ESP8266_TIMER_FRC1_CTRL_ENABLE)) {
        return;
    }

    s->timer_regs[ESP8266_TIMER_FRC1_CTRL / 4] =
        ctrl | ESP8266_TIMER_FRC1_CTRL_INT_STATUS;
    stl_le_p(s->ets_scratch_data + ESP8266_ETS_PENDING_OFF +
             ESP8266_ETS_TIMER_PRIORITY * 4, 1);
    if (ctrl & ESP8266_TIMER_FRC1_CTRL_AUTORELOAD) {
        timer_mod(s->frc1_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                  ESP8266_FRC1_TICK_NS);
    } else {
        s->timer_regs[ESP8266_TIMER_FRC1_CTRL / 4] =
            ctrl & ~ESP8266_TIMER_FRC1_CTRL_ENABLE;
    }
}

static void esp8266_frc1_update(Esp8266SocState *s)
{
    uint32_t ctrl = s->timer_regs[ESP8266_TIMER_FRC1_CTRL / 4];
    uint32_t load = s->timer_regs[ESP8266_TIMER_FRC1_LOAD / 4] &
                    ESP8266_TIMER_FRC1_LOAD_MASK;

    if ((ctrl & ESP8266_TIMER_FRC1_CTRL_ENABLE) && load) {
        stl_le_p(s->ets_scratch_data + ESP8266_ETS_PENDING_OFF +
                 ESP8266_ETS_TIMER_PRIORITY * 4, 1);
        timer_mod(s->frc1_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                  ESP8266_FRC1_TICK_NS);
    } else {
        timer_del(s->frc1_timer);
    }
}

static void esp8266_ets_pump_timer_cb(void *opaque)
{
    Esp8266SocState *s = opaque;
    uint32_t handler = ldl_le_p(s->ets_scratch_data + ESP8266_ETS_TASKS_OFF +
                                ESP8266_ETS_LOOP_PRIORITY * 4);
    uint32_t qlen = ldl_le_p(s->ets_scratch_data + ESP8266_ETS_QLEN_OFF +
                             ESP8266_ETS_LOOP_PRIORITY * 4);

    if (handler && qlen) {
        stl_le_p(s->ets_scratch_data + ESP8266_ETS_PENDING_OFF +
                 ESP8266_ETS_LOOP_PRIORITY * 4, 1);
    }
    timer_mod(s->ets_pump_timer,
              qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
              ESP8266_ETS_PUMP_TICK_NS);
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

static uint64_t esp8266_sdk_time_guard_read(void *opaque, hwaddr addr,
                                            unsigned int size)
{
    Esp8266SocState *s = opaque;
    uint32_t now_us;
    uint32_t now_ms;
    uint64_t value = 0;

    if (size == 0 || addr + size > 8) {
        return 0;
    }
    s->sdk_time_guard_us += ESP8266_TIME_STEP_US;
    now_us = s->sdk_time_guard_us;
    now_ms = s->sdk_time_guard_us / 1000;

    for (unsigned int i = 0; i < size; i++) {
        hwaddr index = addr + i;
        uint8_t byte;

        if (index < sizeof(now_us)) {
            byte = (now_us >> (index * 8)) & 0xff;
        } else {
            index -= sizeof(now_us);
            byte = (now_ms >> (index * 8)) & 0xff;
        }
        value |= (uint64_t)byte << (i * 8);
    }
    return value;
}

static void esp8266_sdk_time_guard_write(void *opaque, hwaddr addr,
                                         uint64_t value, unsigned int size)
{
}

static const MemoryRegionOps esp8266_sdk_time_guard_ops = {
    .read = esp8266_sdk_time_guard_read,
    .write = esp8266_sdk_time_guard_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t esp8266_low_scratch_read(void *opaque, hwaddr addr,
                                         unsigned int size)
{
    Esp8266SocState *s = opaque;
    hwaddr offset = addr;
    uint64_t value = 0;

    if (size == 0 || offset + size > sizeof(s->low_scratch_data)) {
        return 0;
    }
    for (unsigned int i = 0; i < size; i++) {
        value |= (uint64_t)s->low_scratch_data[offset + i] << (i * 8);
    }
    if (size == 4 && offset >= ESP8266_PHY_ROMFUNCS_BASE &&
        offset < ESP8266_PHY_ROMFUNCS_BASE + ESP8266_PHY_ROMFUNCS_SIZE) {
        bool is_code_ptr = (value >= ESP8266_ROM_BASE &&
                            value < ESP8266_ROM_BASE + ESP8266_ROM_SIZE) ||
                           (value >= 0x40100000u && value < 0x40300000u);

        if (!is_code_ptr) {
            return ESP8266_ROM_NOOP_RET;
        }
    }
    return value;
}

static void esp8266_low_scratch_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned int size)
{
    Esp8266SocState *s = opaque;
    hwaddr offset = addr;

    if (size == 0 || offset + size > sizeof(s->low_scratch_data)) {
        return;
    }
    for (unsigned int i = 0; i < size; i++) {
        s->low_scratch_data[offset + i] = (value >> (i * 8)) & 0xff;
    }
}

static const MemoryRegionOps esp8266_low_scratch_ops = {
    .read = esp8266_low_scratch_read,
    .write = esp8266_low_scratch_write,
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

    if (!s->hspi_bus || bytes == 0) {
        return;
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
        uint32_t idle_inputs = ESP8266_GPIO_IDLE_INPUTS & ~s->radio_dio_mask;

        return s->gpio_regs[addr / 4] | idle_inputs | ESP8266_GPIO_BOOT_STRAPS;
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

    switch (addr) {
    case ESP8266_GPIO_OUT:
        s->gpio_regs[addr / 4] = value;
        s->gpio_regs[ESP8266_GPIO_IN / 4] =
            (s->gpio_regs[ESP8266_GPIO_IN / 4] & ~s->gpio_regs[ESP8266_GPIO_ENABLE / 4]) |
            (value & s->gpio_regs[ESP8266_GPIO_ENABLE / 4]);
        return;
    case ESP8266_GPIO_OUT_W1TS:
        s->gpio_regs[ESP8266_GPIO_OUT / 4] |= value;
        s->gpio_regs[ESP8266_GPIO_IN / 4] |=
            value & s->gpio_regs[ESP8266_GPIO_ENABLE / 4];
        return;
    case ESP8266_GPIO_OUT_W1TC:
        s->gpio_regs[ESP8266_GPIO_OUT / 4] &= ~value;
        s->gpio_regs[ESP8266_GPIO_IN / 4] &=
            ~(value & s->gpio_regs[ESP8266_GPIO_ENABLE / 4]);
        return;
    case ESP8266_GPIO_ENABLE:
        s->gpio_regs[addr / 4] = value;
        return;
    case ESP8266_GPIO_ENABLE_W1TS:
        s->gpio_regs[ESP8266_GPIO_ENABLE / 4] |= value;
        return;
    case ESP8266_GPIO_ENABLE_W1TC:
        s->gpio_regs[ESP8266_GPIO_ENABLE / 4] &= ~value;
        return;
    default:
        break;
    }

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
    static unsigned trace_count;

    if (addr == ESP8266_SPI_STATUS) {
        return 0;
    }
    if (addr < sizeof(s->spi_regs) && (addr % 4) == 0) {
        if (ESP8266_TRACE_PERIPH && trace_count++ < 2000 &&
            (addr == ESP8266_SPI_CMD || addr == ESP8266_SPI_ADDR ||
             addr == ESP8266_SPI_RD_STATUS || addr == ESP8266_SPI_USER1 ||
             addr == ESP8266_SPI_USER2 || addr >= ESP8266_SPI_W0)) {
            qemu_log("ESP8266 spi read off=0x%02x size=%u -> 0x%08x\n",
                     (unsigned)addr, size, s->spi_regs[addr / 4]);
        }
        return s->spi_regs[addr / 4];
    }
    return 0;
}

static void esp8266_spi_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned int size)
{
    Esp8266SocState *s = opaque;
    static unsigned trace_count;

    if (addr == ESP8266_SPI_CMD) {
        uint32_t user2 = s->spi_regs[ESP8266_SPI_USER2 / 4];
        uint8_t flash_op = user2 & 0xff;

        if (flash_op == 0) {
            flash_op = extract32(user2, 24, 8);
        }
        if (ESP8266_TRACE_PERIPH && trace_count++ < 5000) {
            qemu_log("ESP8266 spi cmd value=0x%08" PRIx64
                     " user1=0x%08x user2=0x%08x op=0x%02x addr=0x%08x w0=0x%08x\n",
                     value, s->spi_regs[ESP8266_SPI_USER1 / 4],
                     user2, flash_op, s->spi_regs[ESP8266_SPI_ADDR / 4],
                     s->spi_regs[ESP8266_SPI_W0 / 4]);
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
        if (ESP8266_TRACE_PERIPH && trace_count++ < 5000 &&
            (addr == ESP8266_SPI_ADDR || addr == ESP8266_SPI_USER1 ||
             addr == ESP8266_SPI_USER2 || addr >= ESP8266_SPI_W0)) {
            qemu_log("ESP8266 spi write off=0x%02x size=%u value=0x%08" PRIx64 "\n",
                     (unsigned)addr, size, value);
        }
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
    uint64_t flash_addr64 = (uint64_t)sector * ESP8266_FLASH_SECTOR_SIZE;

    if (flash_addr64 <= ESP8266_FLASH_MAP_SIZE - ESP8266_FLASH_SECTOR_SIZE) {
        uint32_t flash_addr = flash_addr64;
        if (ESP8266_TRACE_FLASH_HELPER) {
            qemu_log("ESP8266 flash erase sector=%u addr=0x%06x\n", sector,
                     flash_addr);
        }
        uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
        uint8_t *drom = memory_region_get_ram_ptr(&s->drom);
        memset(irom + flash_addr, 0xff, ESP8266_FLASH_SECTOR_SIZE);
        memset(drom + flash_addr, 0xff, ESP8266_FLASH_SECTOR_SIZE);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ESP8266 flash erase skipped: sector=%u out of map\n",
                      sector);
    }
}

static void esp8266_flash_helper_program(Esp8266SocState *s, uint32_t len)
{
    uint32_t flash_addr = s->flash_helper_addr;
    g_autofree uint8_t *buf = NULL;

    if (len == 0 || flash_addr >= ESP8266_FLASH_MAP_SIZE) {
        return;
    }
    len = MIN(len, ESP8266_FLASH_MAP_SIZE - flash_addr);
    if (ESP8266_TRACE_FLASH_HELPER) {
        qemu_log("ESP8266 flash program addr=0x%06x src=0x%08x len=%u\n",
                 flash_addr, s->flash_helper_src, len);
    }
    buf = g_malloc(len);
    if (s->flash_helper_src < ESP8266_LOW_SCRATCH_SIZE) {
        uint32_t copied = MIN(len, ESP8266_LOW_SCRATCH_SIZE -
                                   s->flash_helper_src);
        memcpy(buf, s->low_scratch_data + s->flash_helper_src, copied);
        if (copied < len) {
            memset(buf + copied, 0xff, len - copied);
        }
    } else if (address_space_read(&address_space_memory, s->flash_helper_src,
                                  MEMTXATTRS_UNSPECIFIED, buf, len) !=
               MEMTX_OK) {
        if (ESP8266_TRACE_FLASH_HELPER) {
            qemu_log("ESP8266 flash program padded unreadable "
                     "src=0x%08x len=%u addr=0x%06x\n",
                     s->flash_helper_src, len, flash_addr);
        }
        memset(buf, 0xff, len);
    }
    uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
    uint8_t *drom = memory_region_get_ram_ptr(&s->drom);
    memcpy(irom + flash_addr, buf, len);
    memcpy(drom + flash_addr, buf, len);
}

static void esp8266_flash_helper_copy_from_flash(Esp8266SocState *s,
                                                 uint32_t len)
{
    uint32_t flash_addr = s->flash_helper_addr & (ESP8266_FLASH_SIZE - 1);
    uint32_t dst_addr = s->flash_helper_read_dst;
    uint8_t *irom = memory_region_get_ram_ptr(&s->irom);
    uint8_t buf[256];

    if (ESP8266_TRACE_FLASH_HELPER) {
        qemu_log("ESP8266 flash read addr=0x%06x dst=0x%08x len=%u\n",
                 flash_addr, dst_addr, len);
    }
    if (len > 64 * KiB) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ESP8266 flash read skipped: addr=0x%06x "
                      "dst=0x%08x len=%u\n",
                      flash_addr, dst_addr, len);
        return;
    }
    while (len) {
        uint32_t chunk = MIN(len, sizeof(buf));

        if (flash_addr + chunk > ESP8266_FLASH_SIZE) {
            chunk = ESP8266_FLASH_SIZE - flash_addr;
        }
        if (chunk == 0) {
            break;
        }
        memcpy(buf, irom + flash_addr, chunk);
        if (dst_addr < ESP8266_LOW_SCRATCH_SIZE) {
            uint32_t copied = MIN(chunk, ESP8266_LOW_SCRATCH_SIZE - dst_addr);

            memcpy(s->low_scratch_data + dst_addr, buf, copied);
            if (copied < chunk) {
                address_space_write(&address_space_memory, dst_addr + copied,
                                    MEMTXATTRS_UNSPECIFIED, buf + copied,
                                    chunk - copied);
            }
        } else {
            address_space_write(&address_space_memory, dst_addr,
                                MEMTXATTRS_UNSPECIFIED, buf, chunk);
        }
        flash_addr = (flash_addr + chunk) & (ESP8266_FLASH_SIZE - 1);
        dst_addr += chunk;
        len -= chunk;
    }
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

    if (ESP8266_TRACE_FLASH_HELPER) {
        qemu_log("ESP8266 flash helper write off=0x%02x value=0x%08" PRIx64
                 " size=%u\n", (unsigned)addr, value, size);
    }
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
    case ESP8266_FLASH_HELPER_READ_ADDR:
        s->flash_helper_addr = value;
        break;
    case ESP8266_FLASH_HELPER_READ_DST:
        s->flash_helper_read_dst = value;
        break;
    case ESP8266_FLASH_HELPER_READ_LEN:
        esp8266_flash_helper_copy_from_flash(s, value);
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
    if (addr == ESP8266_I2C_PHY_CTRL) {
        return ESP8266_I2C_PHY_RESULT;
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
    static unsigned trace_count;

    if (addr == 0 || addr == ESP8266_TIMER_COUNT ||
        addr == ESP8266_TIMER_FRC1_COUNT) {
        static uint64_t timer_us;

        timer_us += ESP8266_TIME_STEP_US;
        if (ESP8266_TRACE_PERIPH && trace_count++ < 1000) {
            qemu_log("ESP8266 timer read off=0x%02x size=%u -> 0x%08" PRIx64 "\n",
                     (unsigned)addr, size, timer_us);
        }
        return timer_us;
    }
    if (addr == ESP8266_TIMER_STATUS) {
        if (ESP8266_TRACE_PERIPH && trace_count++ < 1000) {
            qemu_log("ESP8266 timer read status -> 0x%08x\n",
                     s->timer_regs[addr / 4] | ESP8266_TIMER_STATUS_READY);
        }
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
    static unsigned trace_count;

    if (ESP8266_TRACE_PERIPH && trace_count++ < 1000) {
        qemu_log("ESP8266 timer write off=0x%02x value=0x%08" PRIx64
                 " size=%u\n", (unsigned)addr, value, size);
    }
    if (addr == ESP8266_TIMER_FRC1_INT) {
        s->timer_regs[ESP8266_TIMER_FRC1_CTRL / 4] &=
            ~ESP8266_TIMER_FRC1_CTRL_INT_STATUS;
        return;
    }
    if (addr < sizeof(s->timer_regs) && (addr % 4) == 0) {
        s->timer_regs[addr / 4] = value;
        if (addr == ESP8266_TIMER_FRC1_LOAD ||
            addr == ESP8266_TIMER_FRC1_CTRL) {
            esp8266_frc1_update(s);
        }
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
    static unsigned trace_count;

    if (addr < sizeof(s->sys_regs) && (addr % 4) == 0) {
        if (ESP8266_TRACE_PERIPH && trace_count++ < 1000) {
            qemu_log("ESP8266 sys read off=0x%02x size=%u -> 0x%08x\n",
                     (unsigned)addr, size, s->sys_regs[addr / 4]);
        }
        return s->sys_regs[addr / 4];
    }
    return 0;
}

static void esp8266_sys_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned int size)
{
    Esp8266SocState *s = opaque;
    static unsigned trace_count;

    if (addr < sizeof(s->sys_regs) && (addr % 4) == 0) {
        if (ESP8266_TRACE_PERIPH && trace_count++ < 1000) {
            qemu_log("ESP8266 sys write off=0x%02x value=0x%08" PRIx64
                     " size=%u\n", (unsigned)addr, value, size);
        }
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

    s->cpu[0].env.sregs[PS] = PS_UM | (3 << PS_RING_SHIFT);
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
    size_t exception_table_offset = ESP8266_ROM_EXCEPTION_TABLE - ESP8266_DRAM_BASE;
    size_t flashchip_offset = ESP8266_ROM_FLASHCHIP - ESP8266_DRAM_BASE;
    size_t flashchip_data_offset = ESP8266_ROM_FLASHCHIP_DATA - ESP8266_DRAM_BASE;

    /*
     * The ESP8266 ROM exports flashchip at 0x3fffc714 as a RAM pointer to the
     * flash parameter block. SDK startup rewrites fields in that block after it
     * parses the image header, so seed both the pointer and sane defaults.
     */
    stl_le_p(dram + exception_table_offset + ESP8266_LEVEL1_INTERRUPT_CAUSE * 4,
             ESP8266_ROM_XTOS_L1INT_HANDLER);
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
    static const uint8_t cpu_frequency_80mhz[] = {
        0x02, 0xa8, 0x50,       /* movi a2, 80 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t ets_strlen[] = {
        0x3d, 0x02,             /* mov.n a3, a2 */
        0x46, 0x00, 0x00,       /* j test */
        0x1b, 0x33,             /* loop: addi.n a3, a3, 1 */
        0x42, 0x03, 0x00,       /* test: l8ui a4, a3, 0 */
        0x56, 0x74, 0xff,       /* bnez a4, loop */
        0x20, 0x23, 0xc0,       /* done: sub a2, a3, a2 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t ets_strcmp[] = {
        0x46, 0x01, 0x00,       /* j test */
        0x00, 0x00,             /* padding */
        0x1b, 0x22,             /* loop: addi.n a2, a2, 1 */
        0x1b, 0x33,             /* addi.n a3, a3, 1 */
        0x42, 0x02, 0x00,       /* test: l8ui a4, a2, 0 */
        0x52, 0x03, 0x00,       /* l8ui a5, a3, 0 */
        0x8c, 0x14,             /* beqz.n a4, done */
        0x57, 0x14, 0xf0,       /* beq a4, a5, loop */
        0x50, 0x24, 0xc0,       /* done: sub a2, a4, a5 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t ets_strncmp[] = {
        0x52, 0xa0, 0x00,       /* movi a5, 0 */
        0x57, 0x14, 0x19,       /* beq a4, a5, zero */
        0x42, 0xc4, 0xff,       /* addi a4, a4, -1 */
        0x5a, 0x62,             /* loop: add.n a6, a2, a5 */
        0x5a, 0x73,             /* add.n a7, a3, a5 */
        0x62, 0x06, 0x00,       /* l8ui a6, a6, 0 */
        0x72, 0x07, 0x00,       /* l8ui a7, a7, 0 */
        0x47, 0x15, 0x06,       /* beq a5, a4, done */
        0x8c, 0x36,             /* beqz.n a6, done */
        0x1b, 0x55,             /* addi.n a5, a5, 1 */
        0x77, 0x16, 0xeb,       /* beq a6, a7, loop */
        0x70, 0x56, 0xc0,       /* done: sub a5, a6, a7 */
        0x2d, 0x05,             /* zero: mov.n a2, a5 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t ets_strcpy[] = {
        0x06, 0x1f, 0x00,       /* j strcpy_impl */
    };
    static const uint8_t ets_strncpy[] = {
        0x06, 0x23, 0x00,       /* j strncpy_impl */
    };
    static const uint8_t ets_strncmp_jump[] = {
        0x06, 0x27, 0x00,       /* j strncmp_impl */
    };
    static const uint8_t ets_strcpy_impl[] = {
        0x0c, 0x04, 0x4a, 0x53, 0x52, 0x05, 0x00, 0x4a,
        0x62, 0x52, 0x46, 0x00, 0x1b, 0x44, 0x56, 0x05,
        0xff, 0x0d, 0xf0,
    };
    static const uint8_t ets_strncpy_impl[] = {
        0x0c, 0x07, 0x06, 0x01, 0x00, 0x62, 0x45, 0x00,
        0x1b, 0x77, 0x7a, 0x52, 0x77, 0x14, 0x11, 0x7a,
        0x63, 0x62, 0x06, 0x00, 0x56, 0xd6, 0xfe, 0x2a,
        0x44, 0x62, 0x45, 0x00, 0x1b, 0x55, 0x47, 0x95,
        0xf7, 0x0d, 0xf0,
    };
    static const uint8_t ets_strncmp_impl[] = {
        0x0c, 0x05, 0x57, 0x14, 0x1a, 0x42, 0xc4, 0xff,
        0x5a, 0x62, 0x5a, 0x73, 0x62, 0x06, 0x00, 0x72,
        0x07, 0x00, 0x47, 0x15, 0x07, 0x8c, 0x46, 0x52,
        0xc5, 0x01, 0x77, 0x16, 0xea, 0x70, 0x56, 0xc0,
        0x2d, 0x05, 0x0d, 0xf0,
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
        0x0c, 0x66, 0x40, 0x66, 0x01, 0x0c, 0x18, 0x00,
        0x88, 0x11, 0x8a, 0x66, 0xe0, 0x73, 0x11, 0x7a,
        0x66, 0x29, 0x06, 0x82, 0xa0, 0x80, 0x8a, 0x66,
        0x49, 0x06, 0x0c, 0x09, 0x8a, 0x66, 0x99, 0x06,
        0x8a, 0x66, 0x59, 0x06, 0x8a, 0x66, 0x99, 0x06,
        0x8a, 0x66, 0x99, 0x06, 0x0c, 0x12, 0x0d, 0xf0,
    };
    static const uint8_t ets_post[] = {
        0xe0, 0x62, 0x11, 0x0c, 0x65, 0x40, 0x55, 0x01,
        0x0c, 0x1d, 0x00, 0xdd, 0x11, 0xda, 0x55, 0xed,
        0x05, 0xd2, 0xa0, 0x80, 0xda, 0x55, 0x6a, 0x55,
        0x78, 0x05, 0x16, 0x87, 0x04, 0x5d, 0x0e, 0xda,
        0x55, 0xda, 0x55, 0xda, 0x55, 0x6a, 0x55, 0xa8,
        0x05, 0xbc, 0x9a, 0x5d, 0x0e, 0xda, 0x55, 0xda,
        0x55, 0xda, 0x55, 0xda, 0x55, 0xda, 0x55, 0x6a,
        0x55, 0x88, 0x05, 0xd0, 0x98, 0x11, 0x7a, 0x99,
        0x39, 0x09, 0x49, 0x19, 0x1b, 0x88, 0xa7, 0x28,
        0x02, 0x82, 0xa0, 0x00, 0x89, 0x05, 0x5d, 0x0e,
        0xda, 0x55, 0xda, 0x55, 0x6a, 0x55, 0x88, 0x05,
        0xa7, 0x28, 0x02, 0xc6, 0x00, 0x00, 0x1b, 0x88,
        0x89, 0x05, 0x0c, 0x12, 0x0d, 0xf0, 0x0c, 0x02,
        0x0d, 0xf0,
    };
    static const uint8_t ets_isr_attach[] = {
        0x0c, 0x85,             /* movi.n a5, 8 */
        0x80, 0x55, 0x11,       /* slli a5, a5, 8; table base 0x800 */
        0xd0, 0x62, 0x11,       /* slli a6, a2, 3 */
        0x6a, 0x55,             /* add.n a5, a5, a6 */
        0x28, 0x05,             /* l32i.n a2, a5, 0; return old handler */
        0x39, 0x05,             /* s32i.n a3, a5, 0 */
        0x49, 0x15,             /* s32i.n a4, a5, 4 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t ets_isr_unmask[] = {
        0x30, 0xe4, 0x03,       /* rsr.intenable a3 */
        0x20, 0x33, 0x20,       /* or a3, a3, a2 */
        0x30, 0xe4, 0x13,       /* wsr.intenable a3 */
        0x10, 0x20, 0x00,       /* rsync */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t ets_isr_mask[] = {
        0x30, 0xe4, 0x03,       /* rsr.intenable a3 */
        0x7c, 0xf4,             /* movi.n a4, -1 */
        0x40, 0x22, 0x30,       /* xor a2, a2, a4 */
        0x20, 0x33, 0x10,       /* and a3, a3, a2 */
        0x30, 0xe4, 0x13,       /* wsr.intenable a3 */
        0x10, 0x20, 0x00,       /* rsync */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t ets_isr_attach_jump[] = {
        0x06, 0x1d, 0x00,       /* j ets_isr_attach_impl */
    };
    static const uint8_t ets_isr_mask_jump[] = {
        0x06, 0x21, 0x00,       /* j ets_isr_mask_impl */
    };
    static const uint8_t ets_isr_unmask_jump[] = {
        0x06, 0x25, 0x00,       /* j ets_isr_unmask_impl */
    };
    static const uint8_t xtos_set_exception_handler[] = {
        0x00, 0xc0, 0xff, 0x3f, /* literal: exception handler table */
        0x51, 0xff, 0xff,       /* l32r a5, literal */
        0xe0, 0x62, 0x11,       /* slli a6, a2, 2 */
        0x6a, 0x55,             /* add.n a5, a5, a6 */
        0x28, 0x05,             /* l32i.n a2, a5, 0; return old handler */
        0x39, 0x05,             /* s32i.n a3, a5, 0 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t kernel_exception_jump[] = {
        0x06, 0x16, 0x01,       /* j _xtos_l1int_handler */
    };
    static const uint8_t user_exception_jump[] = {
        0x06, 0x0e, 0x01,       /* j _xtos_l1int_handler */
    };
    static const uint8_t xtos_l1int_handler[] = {
        0x12, 0xc1, 0xb0,       /* addi a1, a1, -80 */
        0x09, 0x01,             /* s32i.n a0, a1, 0 */
        0x29, 0x21,             /* s32i.n a2, a1, 8 */
        0x39, 0x31,             /* s32i.n a3, a1, 12 */
        0x49, 0x41,             /* s32i.n a4, a1, 16 */
        0x59, 0x51,             /* s32i.n a5, a1, 20 */
        0x69, 0x61,             /* s32i.n a6, a1, 24 */
        0x79, 0x71,             /* s32i.n a7, a1, 28 */
        0x89, 0x81,             /* s32i.n a8, a1, 32 */
        0x99, 0x91,             /* s32i.n a9, a1, 36 */
        0xa9, 0xa1,             /* s32i.n a10, a1, 40 */
        0xb9, 0xb1,             /* s32i.n a11, a1, 44 */
        0xc9, 0xc1,             /* s32i.n a12, a1, 48 */
        0xd9, 0xd1,             /* s32i.n a13, a1, 52 */
        0xe9, 0xe1,             /* s32i.n a14, a1, 56 */
        0xf9, 0xf1,             /* s32i.n a15, a1, 60 */
        0x20, 0xe2, 0x03,       /* rsr.interrupt a2 */
        0x30, 0xe4, 0x03,       /* rsr.intenable a3 */
        0x30, 0x22, 0x10,       /* and a2, a2, a3 */
        0x0c, 0x04,             /* movi.n a4, 0 */
        0x0c, 0x15,             /* movi.n a5, 1 */
        0x50, 0x62, 0x10,       /* loop: and a6, a2, a5 */
        0xcc, 0xc6,             /* bnez.n a6, found */
        0x1b, 0x44,             /* addi.n a4, a4, 1 */
        0xf0, 0x55, 0x11,       /* slli a5, a5, 1 */
        0x0c, 0xf6,             /* movi.n a6, 15 */
        0x67, 0x24, 0xf0,       /* blt a4, a6, loop */
        0x06, 0x06, 0x00,       /* j done */
        0x00,                   /* padding */
        0x50, 0xe3, 0x13,       /* found: wsr.intclear a5 */
        0x0c, 0x86,             /* movi.n a6, 8 */
        0x80, 0x66, 0x11,       /* slli a6, a6, 8; table base 0x800 */
        0xd0, 0x74, 0x11,       /* slli a7, a4, 3 */
        0x7a, 0x66,             /* add.n a6, a6, a7 */
        0x08, 0x06,             /* l32i.n a0, a6, 0 */
        0x8c, 0x50,             /* beqz.n a0, done */
        0x28, 0x16,             /* l32i.n a2, a6, 4 */
        0x0c, 0x03,             /* movi.n a3, 0 */
        0xc0, 0x00, 0x00,       /* callx0 a0 */
        0x08, 0x01,             /* done: l32i.n a0, a1, 0 */
        0x28, 0x21,             /* l32i.n a2, a1, 8 */
        0x38, 0x31,             /* l32i.n a3, a1, 12 */
        0x48, 0x41,             /* l32i.n a4, a1, 16 */
        0x58, 0x51,             /* l32i.n a5, a1, 20 */
        0x68, 0x61,             /* l32i.n a6, a1, 24 */
        0x78, 0x71,             /* l32i.n a7, a1, 28 */
        0x88, 0x81,             /* l32i.n a8, a1, 32 */
        0x98, 0x91,             /* l32i.n a9, a1, 36 */
        0xa8, 0xa1,             /* l32i.n a10, a1, 40 */
        0xb8, 0xb1,             /* l32i.n a11, a1, 44 */
        0xc8, 0xc1,             /* l32i.n a12, a1, 48 */
        0xd8, 0xd1,             /* l32i.n a13, a1, 52 */
        0xe8, 0xe1,             /* l32i.n a14, a1, 56 */
        0xf8, 0xf1,             /* l32i.n a15, a1, 60 */
        0x12, 0xc1, 0x50,       /* addi a1, a1, 80 */
        0x00, 0x30, 0x00,       /* rfe */
    };
    static const uint8_t ets_run[] = {
        0x06, 0x2f, 0x00,       /* j ets_run_dispatch */
    };
    static const uint8_t ets_run_dispatch[] = {
        0x1c, 0xf4, 0xe0, 0x74, 0x11, 0x0c, 0x65, 0x40,
        0x55, 0x01, 0x0c, 0x1d, 0x00, 0xdd, 0x11, 0xda,
        0x55, 0xed, 0x05, 0xd2, 0xa0, 0x80, 0xda, 0x55,
        0xda, 0x55, 0x7a, 0x55, 0x88, 0x05, 0x16, 0x38,
        0x04, 0x0b, 0x88, 0x89, 0x05, 0x5d, 0x0e, 0x7a,
        0x55, 0xb8, 0x05, 0xbc, 0x6b, 0x5d, 0x0e, 0xda,
        0x55, 0x7a, 0x55, 0xc8, 0x05, 0xac, 0xcc, 0x5d,
        0x0e, 0xda, 0x55, 0xda, 0x55, 0xda, 0x55, 0x7a,
        0x55, 0xa8, 0x05, 0x9c, 0xea, 0x5d, 0x0e, 0xda,
        0x55, 0xda, 0x55, 0xda, 0x55, 0xda, 0x55, 0x7a,
        0x55, 0x88, 0x05, 0xd0, 0x98, 0x11, 0x9a, 0x2c,
        0x1b, 0x88, 0xa7, 0x28, 0x02, 0x82, 0xa0, 0x00,
        0x89, 0x05, 0xc0, 0x0b, 0x00, 0x0b, 0x44, 0xd6,
        0x74, 0xf9, 0x5d, 0x0e, 0xda, 0x55, 0xda, 0x55,
        0x0c, 0x18, 0x89, 0x15, 0x06, 0xe2, 0xff,
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
    static const uint8_t ets_memset_jump[] = {
        0x06, 0x17, 0x00,       /* j base + 0x60 */
    };
    static const uint8_t ets_memcpy_jump[] = {
        0x06, 0x1b, 0x00,       /* j base + 0x80 */
    };
    static const uint8_t ets_memmove_jump[] = {
        0x06, 0x37, 0x00,       /* j base + 0x100 */
    };
    static const uint8_t ets_memcmp_jump[] = {
        0x06, 0x1f, 0x00,       /* j base + 0xb0 */
    };
    static const uint8_t ets_memset_impl[] = {
        0x5d, 0x02,             /* mov.n a5, a2 */
        0x8c, 0x84,             /* beqz.n a4, done */
        0x32, 0x45, 0x00,       /* loop: s8i a3, a5, 0 */
        0x1b, 0x55,             /* addi.n a5, a5, 1 */
        0x0b, 0x44,             /* addi.n a4, a4, -1 */
        0x56, 0x54, 0xff,       /* bnez a4, loop */
        0x0d, 0xf0,             /* done: ret.n */
    };
    static const uint8_t ets_memcpy_impl[] = {
        0x5d, 0x02,             /* mov.n a5, a2 */
        0x16, 0xe4, 0x00,       /* beqz a4, done */
        0x62, 0x03, 0x00,       /* loop: l8ui a6, a3, 0 */
        0x62, 0x45, 0x00,       /* s8i a6, a5, 0 */
        0x1b, 0x55,             /* addi.n a5, a5, 1 */
        0x1b, 0x33,             /* addi.n a3, a3, 1 */
        0x0b, 0x44,             /* addi.n a4, a4, -1 */
        0x56, 0x04, 0xff,       /* bnez a4, loop */
        0x0d, 0xf0,             /* done: ret.n */
    };
    static const uint8_t ets_memmove_impl[] = {
        0x5d, 0x02,             /* mov.n a5, a2 */
        0xac, 0xe4,             /* beqz.n a4, done */
        0x37, 0x32, 0x1a,       /* bltu a2, a3, forward */
        0x4a, 0x63,             /* add.n a6, a3, a4 */
        0x67, 0xb2, 0x15,       /* bgeu a2, a6, forward */
        0x4a, 0x52,             /* add.n a5, a2, a4 */
        0x4a, 0x33,             /* add.n a3, a3, a4 */
        0x0b, 0x55,             /* backward: addi.n a5, a5, -1 */
        0x0b, 0x33,             /* addi.n a3, a3, -1 */
        0x62, 0x03, 0x00,       /* l8ui a6, a3, 0 */
        0x62, 0x45, 0x00,       /* s8i a6, a5, 0 */
        0x0b, 0x44,             /* addi.n a4, a4, -1 */
        0x56, 0x04, 0xff,       /* bnez a4, backward */
        0x0d, 0xf0,             /* ret.n */
        0x00,                   /* align forward target */
        0x5d, 0x02,             /* forward: mov.n a5, a2 */
        0x62, 0x03, 0x00,       /* loop: l8ui a6, a3, 0 */
        0x62, 0x45, 0x00,       /* s8i a6, a5, 0 */
        0x1b, 0x55,             /* addi.n a5, a5, 1 */
        0x1b, 0x33,             /* addi.n a3, a3, 1 */
        0x42, 0xc4, 0xff,       /* addi a4, a4, -1 */
        0x56, 0xf4, 0xfe,       /* bnez a4, loop */
        0x0d, 0xf0,             /* done: ret.n */
    };
    static const uint8_t ets_memcmp_impl[] = {
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
    static const uint8_t rtc_get_reset_reason[] = {
        0x0c, 0x02,             /* movi.n a2, power-on reset */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t spi_read[] = {
        0x00, 0x14, 0x00, 0x60, /* literal: flash helper base */
        0x51, 0xff, 0xff,       /* l32r a5, . - 4 */
        0x29, 0x45,             /* s32i.n a2, a5, 16 */
        0x39, 0x55,             /* s32i.n a3, a5, 20 */
        0x49, 0x65,             /* s32i.n a4, a5, 24 */
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
    static const uint8_t udivsi3[] = {
        0x0c, 0x04,             /* movi.n a4, 0 */
        0x8c, 0xa3,             /* beqz.n a3, done */
        0x37, 0x32, 0x08,       /* loop: bltu a2, a3, done */
        0x30, 0x22, 0xc0,       /* sub a2, a2, a3 */
        0x1b, 0x44,             /* addi.n a4, a4, 1 */
        0x06, 0xfd, 0xff,       /* j loop */
        0x00,                   /* align done target */
        0x2d, 0x04,             /* done: mov.n a2, a4 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t udivdi3[] = {
        0x56, 0x63, 0x05, 0x56, 0x35, 0x05, 0x16, 0x04,
        0x05, 0x3d, 0x04, 0x16, 0xb3, 0x04, 0x0b, 0x53,
        0x50, 0x63, 0x10, 0xdc, 0x76, 0x0c, 0x04, 0x26,
        0x13, 0x09, 0x30, 0x31, 0x41, 0x1b, 0x44, 0x06,
        0xfd, 0xff, 0x00, 0x00, 0x00, 0x04, 0x40, 0x20,
        0x20, 0x91, 0x0c, 0x03, 0x0d, 0xf0, 0x0c, 0x04,
        0x0c, 0x05, 0x62, 0xa0, 0x20, 0x20, 0x7f, 0x05,
        0xf0, 0x55, 0x11, 0x70, 0x55, 0x20, 0xf0, 0x22,
        0x11, 0xf0, 0x44, 0x11, 0x37, 0x35, 0x04, 0x30,
        0x55, 0xc0, 0x1b, 0x44, 0x0b, 0x66, 0x56, 0x36,
        0xfe, 0x2d, 0x04, 0x0c, 0x03, 0x0d, 0xf0, 0x00,
        0x00, 0x00, 0x0c, 0x02, 0x0c, 0x03, 0x0d, 0xf0,
    };
    static const uint8_t umoddi3[] = {
        0xec, 0x93, 0xec, 0x75, 0xac, 0x54, 0x3d, 0x04,
        0xac, 0x13, 0x0c, 0x04, 0x0c, 0x05, 0x2c, 0x06,
        0x20, 0x7f, 0x05, 0xf0, 0x55, 0x11, 0x70, 0x55,
        0x20, 0xf0, 0x22, 0x11, 0x37, 0x35, 0x02, 0x30,
        0x55, 0xc0, 0x0b, 0x66, 0x56, 0x86, 0xfe, 0x2d,
        0x05, 0x0c, 0x03, 0x0d, 0xf0, 0x0c, 0x02, 0x0c,
        0x03, 0x0d, 0xf0,
    };
    static const uint8_t umodsi3[] = {
        0xac, 0x03, 0x0c, 0x04, 0x0c, 0x05, 0x2c, 0x06,
        0x20, 0x7f, 0x05, 0xf0, 0x55, 0x11, 0x70, 0x55,
        0x20, 0xf0, 0x22, 0x11, 0x37, 0x35, 0x02, 0x30,
        0x55, 0xc0, 0x0b, 0x66, 0x56, 0x86, 0xfe, 0x2d,
        0x05, 0x0d, 0xf0, 0x00, 0x0c, 0x02, 0x0d, 0xf0,
    };
    static const uint8_t udivsi3_alias[] = {
        0x0c, 0x04,             /* movi.n a4, 0 */
        0x8c, 0xa3,             /* beqz.n a3, done */
        0x37, 0x32, 0x08,       /* loop: bltu a2, a3, done */
        0x30, 0x22, 0xc0,       /* sub a2, a2, a3 */
        0x1b, 0x44,             /* addi.n a4, a4, 1 */
        0x06, 0xfd, 0xff,       /* j loop */
        0x00,                   /* align done target */
        0x2d, 0x04,             /* done: mov.n a2, a4 */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t umulsidi3[] = {
        0x12, 0xc1, 0xf0, 0xc9, 0x01, 0xd9, 0x11, 0x20,
        0x40, 0xf4, 0x20, 0x50, 0xf5, 0x30, 0x60, 0xf4,
        0x30, 0x70, 0xf5, 0x60, 0x84, 0x82, 0x60, 0x95,
        0x82, 0x70, 0xa4, 0x82, 0x70, 0xb5, 0x82, 0x9a,
        0xaa, 0x0c, 0x0c, 0x97, 0x3a, 0x02, 0x46, 0x00,
        0x00, 0x0c, 0x1c, 0x00, 0xda, 0x11, 0xa0, 0xa0,
        0xf5, 0xda, 0x28, 0x0c, 0x0d, 0x87, 0x32, 0x03,
        0x86, 0x00, 0x00, 0x00, 0x0c, 0x1d, 0x00, 0xcc,
        0x11, 0xaa, 0x3b, 0xca, 0x33, 0xda, 0x33, 0xc8,
        0x01, 0xd8, 0x11, 0x12, 0xc1, 0x10, 0x0d, 0xf0,
    };
    static const uint8_t phy_get_romfuncs[] = {
        0x22, 0xa1, 0x00,       /* movi a2, ESP8266_PHY_ROMFUNCS_BASE */
        0x0d, 0xf0,             /* ret.n */
    };
    static const uint8_t compat_ret[] = {
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
    memcpy(rom + ESP8266_ROM_ETS_UPDATE_CPU_FREQUENCY - ESP8266_ROM_BASE,
           zero_result, sizeof(zero_result));
    memcpy(rom + ESP8266_ROM_ETS_GET_CPU_FREQUENCY - ESP8266_ROM_BASE,
           cpu_frequency_80mhz, sizeof(cpu_frequency_80mhz));
    memcpy(rom + ESP8266_ROM_ETS_STRCPY - ESP8266_ROM_BASE, ets_strcpy,
           sizeof(ets_strcpy));
    memcpy(rom + ESP8266_ROM_ETS_STRNCPY - ESP8266_ROM_BASE, ets_strncpy,
           sizeof(ets_strncpy));
    memcpy(rom + ESP8266_ROM_ETS_STRNCMP - ESP8266_ROM_BASE, ets_strncmp_jump,
           sizeof(ets_strncmp_jump));
    memcpy(rom + ESP8266_ROM_ETS_STRCPY - ESP8266_ROM_BASE + 0x80,
           ets_strcpy_impl, sizeof(ets_strcpy_impl));
    memcpy(rom + ESP8266_ROM_ETS_STRCPY - ESP8266_ROM_BASE + 0xa0,
           ets_strncpy_impl, sizeof(ets_strncpy_impl));
    memcpy(rom + ESP8266_ROM_ETS_STRCPY - ESP8266_ROM_BASE + 0xd0,
           ets_strncmp_impl, sizeof(ets_strncmp_impl));
    memcpy(rom + ESP8266_ROM_RTC_GET_RESET_REASON - ESP8266_ROM_BASE,
           rtc_get_reset_reason, sizeof(rtc_get_reset_reason));
    memcpy(rom + ESP8266_ROM_ETS_TASK - ESP8266_ROM_BASE, ets_task,
           sizeof(ets_task));
    memcpy(rom + ESP8266_ROM_ETS_RUN - ESP8266_ROM_BASE, ets_run,
           sizeof(ets_run));
    memcpy(rom + ESP8266_ROM_ETS_RUN - ESP8266_ROM_BASE + 0xc0,
           ets_run_dispatch, sizeof(ets_run_dispatch));
    memcpy(rom + ESP8266_ROM_ETS_POST - ESP8266_ROM_BASE, ets_post,
           sizeof(ets_post));
    memcpy(rom + ESP8266_ROM_ETS_ISR_ATTACH - ESP8266_ROM_BASE,
           ets_isr_attach_jump, sizeof(ets_isr_attach_jump));
    memcpy(rom + ESP8266_ROM_ETS_ISR_MASK - ESP8266_ROM_BASE,
           ets_isr_mask_jump, sizeof(ets_isr_mask_jump));
    memcpy(rom + ESP8266_ROM_ETS_ISR_UNMASK - ESP8266_ROM_BASE,
           ets_isr_unmask_jump, sizeof(ets_isr_unmask_jump));
    memcpy(rom + ESP8266_ROM_ETS_ISR_ATTACH_IMPL - ESP8266_ROM_BASE,
           ets_isr_attach, sizeof(ets_isr_attach));
    memcpy(rom + ESP8266_ROM_ETS_ISR_MASK_IMPL - ESP8266_ROM_BASE,
           ets_isr_mask, sizeof(ets_isr_mask));
    memcpy(rom + ESP8266_ROM_ETS_ISR_UNMASK_IMPL - ESP8266_ROM_BASE,
           ets_isr_unmask, sizeof(ets_isr_unmask));
    memcpy(rom + ESP8266_ROM_KERNEL_EXCEPTION_VECTOR - ESP8266_ROM_BASE,
           kernel_exception_jump, sizeof(kernel_exception_jump));
    memcpy(rom + ESP8266_ROM_USER_EXCEPTION_VECTOR - ESP8266_ROM_BASE,
           user_exception_jump, sizeof(user_exception_jump));
    memcpy(rom + ESP8266_ROM_XTOS_SET_EXCEPTION_HANDLER - ESP8266_ROM_BASE - 4,
           xtos_set_exception_handler, sizeof(xtos_set_exception_handler));
    memcpy(rom + ESP8266_ROM_XTOS_L1INT_HANDLER - ESP8266_ROM_BASE,
           xtos_l1int_handler, sizeof(xtos_l1int_handler));
    memcpy(rom + ESP8266_ROM_ETS_BZERO - ESP8266_ROM_BASE, ets_bzero,
           sizeof(ets_bzero));
    memcpy(rom + ESP8266_ROM_ETS_MEMSET - ESP8266_ROM_BASE,
           ets_memset_jump, sizeof(ets_memset_jump));
    memcpy(rom + ESP8266_ROM_ETS_MEMCPY - ESP8266_ROM_BASE,
           ets_memcpy_jump, sizeof(ets_memcpy_jump));
    memcpy(rom + ESP8266_ROM_ETS_MEMMOVE - ESP8266_ROM_BASE,
           ets_memmove_jump, sizeof(ets_memmove_jump));
    memcpy(rom + ESP8266_ROM_ETS_MEMCMP - ESP8266_ROM_BASE,
           ets_memcmp_jump, sizeof(ets_memcmp_jump));
    memcpy(rom + ESP8266_ROM_ETS_MEMSET - ESP8266_ROM_BASE + 0x60,
           ets_memset_impl, sizeof(ets_memset_impl));
    memcpy(rom + ESP8266_ROM_ETS_MEMSET - ESP8266_ROM_BASE + 0x80,
           ets_memcpy_impl, sizeof(ets_memcpy_impl));
    memcpy(rom + ESP8266_ROM_ETS_MEMSET - ESP8266_ROM_BASE + 0xb0,
           ets_memcmp_impl, sizeof(ets_memcmp_impl));
    memcpy(rom + ESP8266_ROM_ETS_MEMSET - ESP8266_ROM_BASE + 0x100,
           ets_memmove_impl, sizeof(ets_memmove_impl));
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
    memcpy(rom + ESP8266_ROM_UDIVSI3 - ESP8266_ROM_BASE,
           udivsi3, sizeof(udivsi3));
    memcpy(rom + ESP8266_ROM_UDIVSI3_ALIAS - ESP8266_ROM_BASE,
           udivsi3_alias, sizeof(udivsi3_alias));
    memcpy(rom + ESP8266_ROM_UDIVDI3 - ESP8266_ROM_BASE,
           udivdi3, sizeof(udivdi3));
    memcpy(rom + ESP8266_ROM_UMODDI3 - ESP8266_ROM_BASE,
           umoddi3, sizeof(umoddi3));
    memcpy(rom + ESP8266_ROM_DIVSI3 - ESP8266_ROM_BASE,
           udivsi3, sizeof(udivsi3));
    memcpy(rom + ESP8266_ROM_UMULSIDI3 - ESP8266_ROM_BASE,
           umulsidi3, sizeof(umulsidi3));
    memcpy(rom + ESP8266_ROM_PHY_GET_ROMFUNCS - ESP8266_ROM_BASE,
           phy_get_romfuncs, sizeof(phy_get_romfuncs));
    memcpy(rom + ESP8266_ROM_STRCMP - ESP8266_ROM_BASE, ets_strcmp,
           sizeof(ets_strcmp));
    memcpy(rom + ESP8266_ROM_STRNCMP - ESP8266_ROM_BASE, ets_strncmp,
           sizeof(ets_strncmp));
    memcpy(rom + ESP8266_ROM_MEMCMP - ESP8266_ROM_BASE,
           ets_memcmp, sizeof(ets_memcmp));
    memcpy(rom + ESP8266_ROM_MEMCPY - ESP8266_ROM_BASE,
           guarded_memcpy, sizeof(guarded_memcpy));
    memcpy(rom + ESP8266_ROM_MEMMOVE - ESP8266_ROM_BASE,
           ets_memmove_impl, sizeof(ets_memmove_impl));
    memcpy(rom + ESP8266_ROM_MEMSET - ESP8266_ROM_BASE,
           guarded_memset, sizeof(guarded_memset));
    memcpy(rom + ESP8266_ROM_STRLEN - ESP8266_ROM_BASE, ets_strlen,
           sizeof(ets_strlen));
    memcpy(rom + ESP8266_ROM_UMODSI3 - ESP8266_ROM_BASE,
           umodsi3, sizeof(umodsi3));
    memcpy(rom + ESP8266_ROM_COMPAT_RET - ESP8266_ROM_BASE,
           compat_ret, sizeof(compat_ret));
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
     * The WiFi SDK is not modeled yet; keep its early null-adjacent probes
     * contained, and provide minimal ETS task/interrupt tables.
     */
    memory_region_init_io(&s->low_scratch, OBJECT(dev),
                          &esp8266_low_scratch_ops, s,
                          "esp8266.low-scratch", ESP8266_LOW_SCRATCH_SIZE);
    memory_region_add_subregion(system_memory, ESP8266_LOW_SCRATCH_BASE,
                                &s->low_scratch);
    for (size_t i = 0; i < ESP8266_PHY_ROMFUNCS_SIZE; i += 4) {
        stl_le_p(s->low_scratch_data + i, ESP8266_ROM_NOOP_RET);
    }
    stl_le_p(s->low_scratch_data + ESP8266_PHY_ROMFUNCS_BASE +
             ESP8266_PHY_ROMFUNC_I2C_WRITEREG,
             ESP8266_ROM_NOOP_RET);
    stl_le_p(s->low_scratch_data + ESP8266_PHY_ROMFUNCS_BASE +
             ESP8266_PHY_ROMFUNC_I2C_WRITEREG_MASK_A,
             ESP8266_ROM_NOOP_RET);
    stl_le_p(s->low_scratch_data + ESP8266_PHY_ROMFUNCS_BASE +
             ESP8266_PHY_ROMFUNC_I2C_READREG,
             ESP8266_ROM_NOOP_RET);
    stl_le_p(s->low_scratch_data + ESP8266_PHY_ROMFUNCS_BASE +
             ESP8266_PHY_ROMFUNC_I2C_WRITEREG_MASK,
             ESP8266_ROM_NOOP_RET);
    stl_le_p(s->low_scratch_data + ESP8266_PHY_ROMFUNCS_BASE +
             ESP8266_PHY_ROMFUNC_NOOP_188,
             ESP8266_ROM_NOOP_RET);
    stl_le_p(s->low_scratch_data + ESP8266_PHY_ROMFUNCS_BASE +
             ESP8266_PHY_ROMFUNC_NOOP_200,
             ESP8266_ROM_NOOP_RET);

    /* DRAM: 0x3FFE8000 (80KB) */
    memory_region_init_ram(&s->dram, OBJECT(dev), "esp8266.dram",
                           ESP8266_DRAM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, ESP8266_DRAM_BASE, &s->dram);
    esp8266_init_dram_state(s);

    memory_region_init_io(&s->sdk_time_guard, OBJECT(dev),
                          &esp8266_sdk_time_guard_ops, s,
                          "esp8266.sdk-time-guard", 8);
    memory_region_add_subregion_overlap(system_memory,
                                        ESP8266_SDK_TIME_COUNTER,
                                        &s->sdk_time_guard, 1);

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
                          "esp8266.flash-helper", 0x20);
    memory_region_add_subregion(system_memory, ESP8266_FLASH_HELPER_BASE,
                                &s->flash_helper);

    memory_region_init_ram_ptr(&s->ets_scratch, OBJECT(dev),
                               "esp8266.ets-scratch",
                               ESP8266_ETS_SCRATCH_SIZE,
                               s->ets_scratch_data);
    memory_region_add_subregion(system_memory, ESP8266_ETS_SCRATCH_BASE,
                                &s->ets_scratch);

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
    s->frc1_timer = timer_new_ns(QEMU_CLOCK_REALTIME,
                                 esp8266_frc1_timer_cb, s);
    s->ets_pump_timer = timer_new_ns(QEMU_CLOCK_REALTIME,
                                     esp8266_ets_pump_timer_cb, s);
    timer_mod(s->ets_pump_timer,
              qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
              ESP8266_ETS_PUMP_TICK_NS);

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

static void esp8266_load_flash_image(Esp8266SocState *s, const uint8_t *data,
                                     size_t len, const char *name)
{
    uint32_t entry = ESP8266_FLASH_BASE;
    const uint8_t *image_data = data;
    size_t image_len = len;
    const char *image_kind = "image";

    esp8266_load_raw_flash(s, data, len);

    if (esp8266_load_image_segments(image_data, image_len, &entry)) {
        qemu_log("ESP8266 %s %s entry=0x%08x\n", image_kind, name, entry);
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
    sx128x_linker_anchor();

    if (!cfg || cfg->type == ESP_RADIO_NONE) {
        return;
    }

    if ((cfg->type != ESP_RADIO_SX127X &&
         cfg->type != ESP_RADIO_SX128X) ||
        cfg->spi_bus != 2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ESP8266: unsupported radio config type=%s spi=%d; "
                      "only sx127x/sx128x on HSPI/radio_spi=2 are wired\n",
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

    DeviceState *radio = qdev_new(esp_radio_qdev_type(cfg->type));
    object_property_add_child(OBJECT(ss), "radio-hspi-cs0", OBJECT(radio));
    qdev_prop_set_uint8(radio, "spi_id", 2);
    if (cfg->type == ESP_RADIO_SX127X) {
        qdev_prop_set_uint8(radio, "cs", 0);
    }
    if (air_chr && cfg->type == ESP_RADIO_SX127X) {
        qdev_prop_set_chr(radio, "air-chardev", air_chr);
    }
    qdev_realize_and_unref(radio, BUS(ss->hspi_bus), &error_fatal);
    ss->hspi_cs = qdev_get_gpio_in_named(radio, SSI_GPIO_CS, 0);
    ss->radio_dio_mask = 0;
    if (cfg->chips[0].dio0 >= 0 && cfg->chips[0].dio0 < 32) {
        ss->radio_dio_mask |= BIT(cfg->chips[0].dio0);
    }
    if (cfg->chips[0].dio1 >= 0 && cfg->chips[0].dio1 < 32) {
        ss->radio_dio_mask |= BIT(cfg->chips[0].dio1);
    }
    if (cfg->chips[0].busy >= 0 && cfg->chips[0].busy < 32) {
        ss->radio_dio_mask |= BIT(cfg->chips[0].busy);
    }

    qemu_set_irq(ss->hspi_cs, 1);
    qemu_log("ESP8266 radio board: type=%s spi=2 nss=%d busy=%d dio0=%d dio1=%d\n",
             esp_radio_type_str(cfg->type), cfg->chips[0].nss,
             cfg->chips[0].busy, cfg->chips[0].dio0, cfg->chips[0].dio1);
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
