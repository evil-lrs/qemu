/*
 * ESP8266 SoC and machine
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
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
#include "hw/irq.h"
#include "hw/qdev-properties.h"
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
#include "elf.h"

#define TYPE_ESP8266_CPU XTENSA_CPU_TYPE_NAME("lx106")
#define ESP8266_FLASH_BASE 0x40200000
#define ESP8266_FLASH_SIZE (1 * MiB)
#define ESP8266_DRAM_BASE 0x3ffe8000
#define ESP8266_DRAM_SIZE (96 * KiB)
#define ESP8266_SRAM_BASE 0x3ff00000
#define ESP8266_SRAM_SIZE (256 * KiB)
#define ESP8266_ROM_BASE 0x40000000
#define ESP8266_ROM_SIZE (64 * KiB)
#define ESP8266_ROM_CACHE_READ_ENABLE 0x4000242c
#define ESP8266_ROM_ETS_PRINTF 0x400024cc
#define ESP8266_ROM_ETS_PUTC 0x40002be8
#define ESP8266_ROM_SPI_READ 0x40004b1c
#define ESP8266_ROM_MUL_OVERFLOW_CHECK 0x4000dcf0
#define ESP8266_ROM_FLASH_SECTOR_COUNT 0x4000e21c
#define ESP8266_ROM_FLASHCHIP 0x3fffc714
#define ESP8266_ROM_FLASHCHIP_DATA 0x3fffc718
#define ESP8266_FLASH_BLOCK_SIZE (64 * KiB)
#define ESP8266_FLASH_SECTOR_SIZE (4 * KiB)
#define ESP8266_FLASH_PAGE_SIZE 256
#define ESP8266_FLASH_STATUS_MASK 0xffff
#define ESP8266_IMAGE_MAGIC 0xe9
#define ESP8266_UART_FIFO 0x00
#define ESP8266_UART_STATUS 0x1c
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
#define ESP8266_TIMER_STATUS 0x128
#define ESP8266_TIMER_STATUS_READY 0x1
#define ESP8266_RTC_TIMER_STATUS (ESP8266_TIMER_STATUS - 0x100)

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

    if (addr < sizeof(s->spi_regs) && (addr % 4) == 0) {
        return s->spi_regs[addr / 4];
    }
    return 0;
}

static void esp8266_spi_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned int size)
{
    Esp8266SocState *s = opaque;

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

static uint64_t esp8266_wifi_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp8266SocState *s = opaque;

    switch (addr) {
    case ESP8266_WIFI_STATUS:
        return s->wifi_regs[addr / 4] | ESP8266_WIFI_STATUS_READY;
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
}

static void esp8266_init_rom_stubs(Esp8266SocState *s)
{
    uint8_t *rom = memory_region_get_ram_ptr(&s->rom);
    static const uint8_t ets_printf[] = {
        0x0c, 0x02,             /* movi.n a2, 0 */
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
    static const uint8_t flash_sector_count[] = {
        0x0c, 0x12,             /* movi.n a2, 1 */
        0x2a, 0x22,             /* add.n a2, a2, a2 */
        0x2a, 0x22,             /* add.n a2, a2, a2 */
        0x2a, 0x22,             /* add.n a2, a2, a2 */
        0x2a, 0x22,             /* add.n a2, a2, a2 */
        0x2a, 0x22,             /* add.n a2, a2, a2 */
        0x2a, 0x22,             /* add.n a2, a2, a2 */
        0x2a, 0x22,             /* add.n a2, a2, a2 */
        0x2a, 0x22,             /* add.n a2, a2, a2 */
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

    memcpy(rom + ESP8266_ROM_ETS_PRINTF - ESP8266_ROM_BASE, ets_printf,
           sizeof(ets_printf));
    memcpy(rom + ESP8266_ROM_CACHE_READ_ENABLE - ESP8266_ROM_BASE,
           cache_read_enable, sizeof(cache_read_enable));
    memcpy(rom + ESP8266_ROM_ETS_PUTC - ESP8266_ROM_BASE - 4, ets_putc,
           sizeof(ets_putc));
    memcpy(rom + ESP8266_ROM_SPI_READ - ESP8266_ROM_BASE - 4, spi_read,
           sizeof(spi_read));
    memcpy(rom + ESP8266_ROM_FLASH_SECTOR_COUNT - ESP8266_ROM_BASE,
           flash_sector_count, sizeof(flash_sector_count));
    memcpy(rom + ESP8266_ROM_MUL_OVERFLOW_CHECK - ESP8266_ROM_BASE,
           mul_overflow_check, sizeof(mul_overflow_check));
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

    /* IROM (Mapped from Flash): 0x40200000 (1MB typical) */
    memory_region_init_ram(&s->irom, OBJECT(dev), "esp8266.irom", 1 * MiB, &error_fatal);
    memory_region_add_subregion(system_memory, 0x40200000, &s->irom);

    /* DROM (Mapped from Flash): 0x3f400000 (1MB typical) */
    memory_region_init_ram(&s->drom, OBJECT(dev), "esp8266.drom", 1 * MiB, &error_fatal);
    memory_region_add_subregion(system_memory, 0x3f400000, &s->drom);

    if (serial_hd(0)) {
        qemu_chr_fe_init(&s->uart0_chr, serial_hd(0), &error_abort);
    }

    memory_region_init_io(&s->uart0, OBJECT(dev), &esp8266_uart_ops, s,
                          "esp8266.uart0", 0x100);
    memory_region_add_subregion(system_memory, 0x60000000, &s->uart0);

    memory_region_init_io(&s->gpio, OBJECT(dev), &esp8266_gpio_ops, s,
                          "esp8266.gpio", 0x100);
    memory_region_add_subregion(system_memory, 0x60000300, &s->gpio);

    memory_region_init_io(&s->spi, OBJECT(dev), &esp8266_spi_ops, s,
                          "esp8266.spi", 0x100);
    memory_region_add_subregion(system_memory, 0x60000200, &s->spi);

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
    void *irom = memory_region_get_ram_ptr(&s->irom);
    size_t copy_len = MIN(len, (size_t)ESP8266_FLASH_SIZE);

    memcpy(irom, data, copy_len);
    if (copy_len < ESP8266_FLASH_SIZE) {
        memset((uint8_t *)irom + copy_len, 0xff, ESP8266_FLASH_SIZE - copy_len);
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

    esp8266_load_raw_flash(s, data, len);

    if (esp8266_load_image_segments(data, len, &entry)) {
        qemu_log("ESP8266 image %s entry=0x%08x\n", name, entry);
    } else {
        warn_report("ESP8266 image %s is not a parseable ESP8266 image; "
                    "jumping to 0x%08x", name, entry);
    }

    esp8266_uart_puts(s, "rst:0x1 (POWERON_RESET),boot:0x0 (qemu)\r\n");
    esp8266_uart_puts(s, "eboot: qemu minimal ESP8266 image loader\r\n");
    s->boot_entry = entry;
    s->boot_loaded = true;
    esp8266_apply_boot_state(s);
}

static void esp8266_machine_init(MachineState *machine)
{
    Esp8266MachineState *ms = ESP8266_MACHINE(machine);
    DeviceState *soc = qdev_new(TYPE_ESP8266_SOC);
    Esp8266SocState *ss;

    esp_radio_config_log("ESP8266", ms->radio_config);
    if (ms->radio_config) {
        qemu_log("ESP8266: radio-config accepted for validation metadata; "
                 "radio SSI wiring is not implemented yet\n");
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(soc), &error_fatal);
    ss = ESP8266_SOC(soc);
    qemu_register_reset(esp8266_boot_reset, ss);
    cpu_reset(CPU(&ss->cpu[0]));

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
