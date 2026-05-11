#ifndef HW_XTENSA_ESP8266_H
#define HW_XTENSA_ESP8266_H

#include "hw/sysbus.h"
#include "hw/xtensa/xtensa_memory.h"
#include "hw/char/serial.h"
#include "hw/timer/esp32_frc_timer.h"
#include "hw/ssi/esp32_spi.h"
#include "hw/gpio/esp32_gpio.h"

#define ESP8266_CPU_COUNT 1

typedef struct Esp8266SocState {
    SysBusDevice parent_obj;

    MemoryRegion dram;
    MemoryRegion iram;
    MemoryRegion irom;
    MemoryRegion drom;

    XtensaCPU cpu[ESP8266_CPU_COUNT];
} Esp8266SocState;

#define TYPE_ESP8266_SOC "xtensa.esp8266"
#define ESP8266_SOC(obj) OBJECT_CHECK(Esp8266SocState, (obj), TYPE_ESP8266_SOC)

#endif
