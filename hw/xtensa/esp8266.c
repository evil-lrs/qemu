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
#define ESP8266_IMAGE_MAGIC 0xe9
#define ESP8266_UART_FIFO 0x00
#define ESP8266_UART_STATUS 0x1c

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

static void esp8266_uart_puts(Esp8266SocState *s, const char *str)
{
    if (qemu_chr_fe_backend_connected(&s->uart0_chr)) {
        qemu_chr_fe_write_all(&s->uart0_chr, (const uint8_t *)str, strlen(str));
    } else {
        qemu_log("%s", str);
    }
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
    memory_region_init_ram(&s->dram, OBJECT(dev), "esp8266.dram", 80 * KiB, &error_fatal);
    memory_region_add_subregion(system_memory, 0x3ffe8000, &s->dram);

    /*
     * IRAM: SDK eboot images commonly place their first segment at
     * 0x4010f000, so expose the full 64 KiB 0x40100000..0x4010ffff window.
     */
    memory_region_init_ram(&s->iram, OBJECT(dev), "esp8266.iram", 64 * KiB, &error_fatal);
    memory_region_add_subregion(system_memory, 0x40100000, &s->iram);

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

    /* Unimplemented peripherals to avoid crashes */
    create_unimplemented_device("esp8266.spi",   0x60000200, 0x100);
    create_unimplemented_device("esp8266.gpio",  0x60000300, 0x100);
    create_unimplemented_device("esp8266.timer", 0x60000600, 0x100);
    create_unimplemented_device("esp8266.rtc",   0x60000700, 0x100);
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
    CPUState *cs = CPU(&s->cpu[0]);
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
    cpu_set_pc(cs, entry);
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
