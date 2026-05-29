#ifndef HW_XTENSA_ESP8266_H
#define HW_XTENSA_ESP8266_H

#include "hw/sysbus.h"
#include "hw/xtensa/xtensa_memory.h"
#include "chardev/char-fe.h"

#define ESP8266_CPU_COUNT 1

typedef struct Esp8266SocState {
    SysBusDevice parent_obj;

    MemoryRegion dram;
    MemoryRegion iram;
    MemoryRegion irom;
    MemoryRegion drom;
    MemoryRegion uart0;

    CharBackend uart0_chr;
    XtensaCPU cpu[ESP8266_CPU_COUNT];
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
