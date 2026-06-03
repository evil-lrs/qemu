#ifndef HW_MISC_ESP_RADIO_CONFIG_H
#define HW_MISC_ESP_RADIO_CONFIG_H

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qom/object.h"

/*
 * Helpers to expose the `radio-config=<path>` and `radio-air-chardev=<id>`
 * string properties on ESP32-family QEMU machines. The radio chip type, SPI
 * bus, pin assignments and optional I2C peripherals are all described inside
 * the JSON pointed to by `radio-config=`; there is no separate machine-level
 * "radio chip" knob. See docs/hardware-config.md.
 *
 * Each machine struct must contain `char *radio_config;` and
 * `char *radio_air_chardev;` fields.
 *
 * Use ESP_RADIO_OPTIONS_DEFINE_ACCESSORS at file scope to emit getters/setters,
 * and ESP_RADIO_OPTIONS_ADD_PROPS inside machine_class_init. Call
 * esp_radio_config_log() from machine_init so tests can detect the option:
 *
 *     "<CHIP> radio config: <path>"
 */

#define ESP_RADIO_OPTIONS_DEFINE_ACCESSORS(prefix, MachineStateT, CAST_MACRO) \
    static char *prefix##_get_radio_config(Object *obj, Error **errp)         \
    {                                                                         \
        MachineStateT *ms = CAST_MACRO(obj);                                  \
        return g_strdup(ms->radio_config);                                    \
    }                                                                         \
    static void prefix##_set_radio_config(Object *obj, const char *value,     \
                                          Error **errp)                       \
    {                                                                         \
        MachineStateT *ms = CAST_MACRO(obj);                                  \
        g_free(ms->radio_config);                                             \
        ms->radio_config = g_strdup(value);                                   \
    }                                                                         \
    static char *prefix##_get_radio_air_chardev(Object *obj, Error **errp)    \
    {                                                                         \
        MachineStateT *ms = CAST_MACRO(obj);                                  \
        return g_strdup(ms->radio_air_chardev);                               \
    }                                                                         \
    static void prefix##_set_radio_air_chardev(Object *obj, const char *value,\
                                               Error **errp)                  \
    {                                                                         \
        MachineStateT *ms = CAST_MACRO(obj);                                  \
        g_free(ms->radio_air_chardev);                                        \
        ms->radio_air_chardev = g_strdup(value);                              \
    }

#define ESP_RADIO_OPTIONS_ADD_PROPS(oc, prefix)                               \
    do {                                                                      \
        object_class_property_add_str((oc), "radio-config",                   \
            prefix##_get_radio_config, prefix##_set_radio_config);            \
        object_class_property_set_description((oc), "radio-config",           \
            "Path to JSON radio board configuration file. See "               \
            "docs/hardware-config.md for the schema.");                       \
        object_class_property_add_str((oc), "radio-air-chardev",              \
            prefix##_get_radio_air_chardev, prefix##_set_radio_air_chardev);  \
        object_class_property_set_description((oc), "radio-air-chardev",      \
            "Chardev ID for the primary radio air-bus connection");           \
    } while (0)

static inline void esp_radio_config_log(const char *chip, const char *path)
{
    if (path) {
        qemu_log("%s radio config: %s\n", chip, path);
    }
}

#endif /* HW_MISC_ESP_RADIO_CONFIG_H */
