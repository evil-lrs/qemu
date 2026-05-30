#ifndef HW_XTENSA_ESP8266_H
#define HW_XTENSA_ESP8266_H

#include "hw/sysbus.h"
#include "hw/xtensa/xtensa_memory.h"
#include "chardev/char-fe.h"
#include "target/xtensa/cpu.h"

#define ESP8266_CPU_COUNT 1
#define ESP8266_GPIO_MMIO_WORDS (0x100 / 4)
#define ESP8266_I2C_MMIO_WORDS (0x400 / 4)
#define ESP8266_RTC_MMIO_WORDS (0x100 / 4)
#define ESP8266_IOMUX_MMIO_WORDS (0x100 / 4)
#define ESP8266_WIFI_MMIO_WORDS (0x2000 / 4)

typedef struct Esp8266SocState {
    SysBusDevice parent_obj;

    MemoryRegion dram;
    MemoryRegion sram;
    MemoryRegion dport;
    MemoryRegion iram;
    MemoryRegion rom;
    MemoryRegion irom;
    MemoryRegion drom;
    MemoryRegion uart0;
    MemoryRegion gpio;
    MemoryRegion i2c;
    MemoryRegion rtc;
    MemoryRegion iomux;
    MemoryRegion wifi;

    CharBackend uart0_chr;
    XtensaCPU cpu[ESP8266_CPU_COUNT];
    uint32_t gpio_regs[ESP8266_GPIO_MMIO_WORDS];
    uint32_t i2c_regs[ESP8266_I2C_MMIO_WORDS];
    uint32_t rtc_regs[ESP8266_RTC_MMIO_WORDS];
    uint32_t iomux_regs[ESP8266_IOMUX_MMIO_WORDS];
    uint32_t wifi_regs[ESP8266_WIFI_MMIO_WORDS];
    uint32_t boot_entry;
    bool boot_loaded;
} Esp8266SocState;

typedef struct Esp8266MachineState {
    MachineState parent;

    char *radio_config;
    char *radio_air_chardev;
} Esp8266MachineState;

#define TYPE_ESP8266_SOC "xtensa.esp8266"
#define ESP8266_SOC(obj) OBJECT_CHECK(Esp8266SocState, (obj), TYPE_ESP8266_SOC)

#define TYPE_ESP8266_MACHINE MACHINE_TYPE_NAME("esp8266")
#define ESP8266_MACHINE(obj) \
    OBJECT_CHECK(Esp8266MachineState, (obj), TYPE_ESP8266_MACHINE)

#endif
