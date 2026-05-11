/*
 * ESP8266 SoC and machine
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/xtensa/xtensa_memory.h"
#include "hw/misc/unimp.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/xtensa/esp8266.h"
#include "hw/ssi/sx127x.h"
#include "hw/ssi/sx128x.h"
#include "core-lx106/core-isa.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "sysemu/cpus.h"
#include "sysemu/runstate.h"
#include "sysemu/blockdev.h"
#include "sysemu/block-backend.h"
#include "exec/exec-all.h"
#include "elf.h"

#include "hw/ssi/ssi.h"
#include "hw/block/flash.h"

#define TYPE_ESP8266_CPU XTENSA_CPU_TYPE_NAME("lx106")

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

    /* IRAM: 0x40100000 (32KB) */
    memory_region_init_ram(&s->iram, OBJECT(dev), "esp8266.iram", 32 * KiB, &error_fatal);
    memory_region_add_subregion(system_memory, 0x40100000, &s->iram);

    /* IROM (Mapped from Flash): 0x40200000 (1MB typical) */
    memory_region_init_ram(&s->irom, OBJECT(dev), "esp8266.irom", 1 * MiB, &error_fatal);
    memory_region_add_subregion(system_memory, 0x40200000, &s->irom);

    /* DROM (Mapped from Flash): 0x3f400000 (1MB typical) */
    memory_region_init_ram(&s->drom, OBJECT(dev), "esp8266.drom", 1 * MiB, &error_fatal);
    memory_region_add_subregion(system_memory, 0x3f400000, &s->drom);

    /* Unimplemented peripherals to avoid crashes */
    create_unimplemented_device("esp8266.uart0", 0x60000000, 0x100);
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

static void esp8266_machine_init(MachineState *machine)
{
    DeviceState *soc = qdev_new(TYPE_ESP8266_SOC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(soc), &error_fatal);

    if (machine->kernel_filename) {
        /* Direct image loading for testing */
        load_image_targphys(machine->kernel_filename, 0x40200000, 1 * MiB);
        /* Also set PC to the entry point if we don't have a bootrom */
        CPUState *cs = CPU(&ESP8266_SOC(soc)->cpu[0]);
        cpu_set_pc(cs, 0x40200000);
    }

    /* If flash is provided as -drive file=..., we load it into our IROM/DROM buffers */
    DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        BlockBackend *blk = blk_by_legacy_dinfo(dinfo);
        uint64_t len = blk_getlength(blk);
        if (len > 0) {
            void *ptr = memory_region_get_ram_ptr(&ESP8266_SOC(soc)->irom);
            if (len > 1 * MiB) len = 1 * MiB;
            blk_pread(blk, 0, len, ptr, 0);
            /* Set PC to eboot or app start if we loaded a flash image */
            CPUState *cs = CPU(&ESP8266_SOC(soc)->cpu[0]);
            cpu_set_pc(cs, 0x40200000);
        }
    }
}

static void esp8266_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "ESP8266 (LX106)";
    mc->init = esp8266_machine_init;
    mc->default_cpu_type = TYPE_ESP8266_CPU;
    mc->block_default_type = IF_MTD;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

static const TypeInfo esp8266_machine_type_info = {
    .name = MACHINE_TYPE_NAME("esp8266"),
    .parent = TYPE_MACHINE,
    .class_init = esp8266_machine_class_init,
};

static void esp8266_register_types(void)
{
    type_register_static(&esp8266_soc_type_info);
    type_register_static(&esp8266_machine_type_info);
}

type_init(esp8266_register_types)
