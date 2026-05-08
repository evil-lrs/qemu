/*
 * ESP32 GPIO emulation
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32_gpio.h"

#define GPIO_OUT            0x0004
#define GPIO_OUT_W1TS       0x0008
#define GPIO_OUT_W1TC       0x000c
#define GPIO_OUT1           0x0010
#define GPIO_OUT1_W1TS      0x0014
#define GPIO_OUT1_W1TC      0x0018
#define GPIO_ENABLE         0x0020
#define GPIO_ENABLE_W1TS    0x0024
#define GPIO_ENABLE_W1TC    0x0028
#define GPIO_ENABLE1        0x002c
#define GPIO_ENABLE1_W1TS   0x0030
#define GPIO_ENABLE1_W1TC   0x0034
#define GPIO_IN             0x003c
#define GPIO_IN1            0x0040

static bool esp32_gpio_log_pin(unsigned pin)
{
    switch (pin) {
    case 13:
    case 25:
    case 26:
    case 27:
    case 32:
    case 33:
    case 34:
    case 36:
    case 37:
    case 39:
        return true;
    default:
        return false;
    }
}
static bool esp32_elrs_trace_enabled(void)
{
    return getenv("QEMU_ESP32_ELRS_TRACE") != NULL;
}

static void esp32_gpio_set_pin(Esp32GpioState *s, unsigned pin, int level)
{
    qemu_set_irq(s->gpio_out[pin], level);

    if (esp32_elrs_trace_enabled() && esp32_gpio_log_pin(pin)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ESP32_GPIO: GPIO%u output %s\n",
                      pin, level ? "high" : "low");
    }
}

static void esp32_gpio_set_input(void *opaque, int n, int level)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    if (n < 32) {
        if (level) {
            s->in_level |= BIT(n);
        } else {
            s->in_level &= ~BIT(n);
        }
    } else if (n < ESP32_GPIO_PIN_COUNT) {
        if (level) {
            s->in_level1 |= BIT(n - 32);
        } else {
            s->in_level1 &= ~BIT(n - 32);
        }
    }
}

static void esp32_gpio_update_outputs(Esp32GpioState *s, uint32_t changed)
{
    for (unsigned pin = 0; pin < 32; ++pin) {
        if (changed & BIT(pin)) {
            esp32_gpio_set_pin(s, pin, (s->out & BIT(pin)) != 0);
        }
    }
}

static uint32_t esp32_gpio_input0(Esp32GpioState *s)
{
    /*
     * If enabled as output, return output value.
     * If disabled as output, return external input value ORed with pull-up.
     * The default pull-up behavior is kept by initializing in_level to ~0.
     */
    return (s->out & s->enable) | (s->in_level & ~s->enable);
}

static uint32_t esp32_gpio_input1(Esp32GpioState *s)
{
    return (s->out1 & s->enable1) | (s->in_level1 & ~s->enable1 & 0xff);
}

static uint64_t esp32_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    uint64_t r = 0;
    switch (addr) {
    case A_GPIO_STRAP:
        r = s->strap_mode;
        break;
    case GPIO_OUT:
        r = s->out;
        break;
    case GPIO_OUT1:
        r = s->out1;
        break;
    case GPIO_ENABLE:
        r = s->enable;
        break;
    case GPIO_ENABLE1:
        r = s->enable1;
        break;
    case GPIO_IN:
        r = esp32_gpio_input0(s);
        break;
    case GPIO_IN1:
        r = esp32_gpio_input1(s);
        break;

    default:
        if (addr + size <= sizeof(s->regs)) {
            r = s->regs[addr / sizeof(uint32_t)];
        }
        break;
    }
    if ((addr == GPIO_IN || addr == GPIO_IN1) && s->input_log_count < 16) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ESP32_GPIO: read %s -> 0x%08" PRIx64
                      " enable=0x%08x/0x%08x out=0x%08x/0x%08x\n",
                      addr == GPIO_IN ? "GPIO_IN" : "GPIO_IN1",
                      r, s->enable, s->enable1, s->out, s->out1);
        s->input_log_count++;
    }
    return r;
}

static void esp32_gpio_write(void *opaque, hwaddr addr,
                       uint64_t value, unsigned int size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    uint32_t old_out = s->out;
    uint32_t old_out1 = s->out1;
    uint32_t v = value;

    if (addr + size <= sizeof(s->regs)) {
        s->regs[addr / sizeof(uint32_t)] = v;
    }

    switch (addr) {
    case GPIO_OUT:
        s->out = v;
        break;
    case GPIO_OUT_W1TS:
        s->out |= v;
        break;
    case GPIO_OUT_W1TC:
        s->out &= ~v;
        break;
    case GPIO_OUT1:
        s->out1 = v;
        break;
    case GPIO_OUT1_W1TS:
        s->out1 |= v;
        break;
    case GPIO_OUT1_W1TC:
        s->out1 &= ~v;
        break;
    case GPIO_ENABLE:
        s->enable = v;
        break;
    case GPIO_ENABLE_W1TS:
        s->enable |= v;
        break;
    case GPIO_ENABLE_W1TC:
        s->enable &= ~v;
        break;
    case GPIO_ENABLE1:
        s->enable1 = v;
        break;
    case GPIO_ENABLE1_W1TS:
        s->enable1 |= v;
        break;
    case GPIO_ENABLE1_W1TC:
        s->enable1 &= ~v;
        break;
    default:
        break;
    }

    if (s->out != old_out) {
        esp32_gpio_update_outputs(s, s->out ^ old_out);
    }
    if (s->out1 != old_out1) {
        uint32_t changed = (s->out1 ^ old_out1) & 0xff;
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (changed & BIT(bit)) {
                esp32_gpio_set_pin(s, 32 + bit, (s->out1 & BIT(bit)) != 0);
            }
        }
    }
}

static const MemoryRegionOps uart_ops = {
    .read =  esp32_gpio_read,
    .write = esp32_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_gpio_reset_hold(Object *obj, ResetType type)
{
    Esp32GpioState *s = ESP32_GPIO(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->out = ~0;
    s->out1 = ~0;
    s->enable = 0;
    s->enable1 = 0;
    s->in_level = ~0;
    s->in_level1 = ~0;
    s->input_log_count = 0;

    for (unsigned pin = 0; pin < ESP32_GPIO_PIN_COUNT; ++pin) {
        qemu_set_irq(s->gpio_out[pin], 1);
    }
}

static void esp32_gpio_realize(DeviceState *dev, Error **errp)
{
}

static void esp32_gpio_init(Object *obj)
{
    Esp32GpioState *s = ESP32_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /* Set the default value for the strap_mode property */
    object_property_set_int(obj, "strap_mode", ESP32_STRAP_MODE_FLASH_BOOT, &error_fatal);

    memory_region_init_io(&s->iomem, obj, &uart_ops, s,
                          TYPE_ESP32_GPIO, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), s->gpio_out, ESP32_GPIO_OUT_GPIO, ESP32_GPIO_PIN_COUNT);
    qdev_init_gpio_in(DEVICE(obj), esp32_gpio_set_input, ESP32_GPIO_PIN_COUNT);
}

static Property esp32_gpio_properties[] = {
    DEFINE_PROP_UINT32("strap_mode", Esp32GpioState, strap_mode, ESP32_STRAP_MODE_FLASH_BOOT),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32_gpio_reset_hold;
    dc->realize = esp32_gpio_realize;
    device_class_set_props(dc, esp32_gpio_properties);
}

static const TypeInfo esp32_gpio_info = {
    .name = TYPE_ESP32_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32GpioState),
    .instance_init = esp32_gpio_init,
    .class_init = esp32_gpio_class_init
};

static void esp32_gpio_register_types(void)
{
    type_register_static(&esp32_gpio_info);
}

type_init(esp32_gpio_register_types)
