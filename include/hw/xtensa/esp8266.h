#ifndef HW_XTENSA_ESP8266_H
#define HW_XTENSA_ESP8266_H

#include "hw/sysbus.h"
#include "hw/xtensa/xtensa_memory.h"
#include "chardev/char-fe.h"
#include "hw/ssi/ssi.h"
#include "target/xtensa/cpu.h"

#define ESP8266_CPU_COUNT 1
#define ESP8266_GPIO_MMIO_WORDS (0x100 / 4)
#define ESP8266_SPI_MMIO_WORDS (0x100 / 4)
#define ESP8266_I2C_MMIO_WORDS (0x800 / 4)
#define ESP8266_TIMER_MMIO_WORDS (0x300 / 4)
#define ESP8266_RTC_MMIO_WORDS (0x100 / 4)
#define ESP8266_IOMUX_MMIO_WORDS (0x100 / 4)
#define ESP8266_WIFI_MMIO_WORDS (0x2000 / 4)
#define ESP8266_HSPI_MMIO_WORDS (0x100 / 4)
#define ESP8266_SYS_MMIO_WORDS (0x100 / 4)

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
    MemoryRegion hspi;
    MemoryRegion gpio;
    MemoryRegion spi;
    MemoryRegion i2c;
    MemoryRegion timer0;
    MemoryRegion timer;
    MemoryRegion rtc;
    MemoryRegion iomux;
    MemoryRegion sys;
    MemoryRegion wifi;

    CharBackend uart0_chr;
    XtensaCPU cpu[ESP8266_CPU_COUNT];
    SSIBus *hspi_bus;
    qemu_irq hspi_cs;
    uint32_t gpio_regs[ESP8266_GPIO_MMIO_WORDS];
    uint32_t spi_regs[ESP8266_SPI_MMIO_WORDS];
    uint32_t i2c_regs[ESP8266_I2C_MMIO_WORDS];
    uint32_t timer_regs[ESP8266_TIMER_MMIO_WORDS];
    uint32_t rtc_regs[ESP8266_RTC_MMIO_WORDS];
    uint32_t iomux_regs[ESP8266_IOMUX_MMIO_WORDS];
    uint32_t wifi_regs[ESP8266_WIFI_MMIO_WORDS];
    uint32_t hspi_regs[ESP8266_HSPI_MMIO_WORDS];
    uint32_t sys_regs[ESP8266_SYS_MMIO_WORDS];
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
