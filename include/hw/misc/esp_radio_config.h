#ifndef HW_MISC_ESP_RADIO_CONFIG_H
#define HW_MISC_ESP_RADIO_CONFIG_H

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qom/object.h"

/*
 * Helpers to expose `radio-config=<path>` and `radio-chip=<name>` string
 * properties on ESP32-family QEMU machines. Each machine struct must contain
 * `char *radio_config;` and `char *radio_chip;` fields.
 *
 * Use ESP_RADIO_OPTIONS_DEFINE_ACCESSORS at file scope to emit getters/setters,
 * and ESP_RADIO_OPTIONS_ADD_PROPS inside machine_class_init to register both
 * properties. Call esp_radio_log_options() from machine_init so tests can
 * detect the options via uniform log lines:
 *
 *     "<CHIP> radio config: <path>"
 *     "<CHIP> radio chip: <name>"
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
    static char *prefix##_get_radio_chip(Object *obj, Error **errp)           \
    {                                                                         \
        MachineStateT *ms = CAST_MACRO(obj);                                  \
        return g_strdup(ms->radio_chip);                                      \
    }                                                                         \
    static void prefix##_set_radio_chip(Object *obj, const char *value,       \
                                        Error **errp)                         \
    {                                                                         \
        MachineStateT *ms = CAST_MACRO(obj);                                  \
        g_free(ms->radio_chip);                                               \
        ms->radio_chip = g_strdup(value);                                     \
    }

#define ESP_RADIO_OPTIONS_ADD_PROPS(oc, prefix)                               \
    do {                                                                      \
        object_class_property_add_str((oc), "radio-config",                   \
            prefix##_get_radio_config, prefix##_set_radio_config);            \
        object_class_property_set_description((oc), "radio-config",           \
            "Path to JSON radio board configuration file");                   \
        object_class_property_add_str((oc), "radio-chip",                     \
            prefix##_get_radio_chip, prefix##_set_radio_chip);                \
        object_class_property_set_description((oc), "radio-chip",             \
            "Radio chip identifier (e.g. sx127x, sx128x). "                   \
            "Selects which radio device QEMU instantiates.");                 \
    } while (0)

/* Backwards-compat aliases for the radio-config-only helpers. */
#define ESP_RADIO_CONFIG_DEFINE_ACCESSORS ESP_RADIO_OPTIONS_DEFINE_ACCESSORS
#define ESP_RADIO_CONFIG_ADD_PROP(oc, prefix) ESP_RADIO_OPTIONS_ADD_PROPS(oc, prefix)

static inline void esp_radio_config_log(const char *chip, const char *path)
{
    if (path) {
        qemu_log("%s radio config: %s\n", chip, path);
    }
}

static inline void esp_radio_chip_log(const char *chip, const char *name)
{
    if (name) {
        qemu_log("%s radio chip: %s\n", chip, name);
    }
}

/*
 * Resolve the effective radio chip selection for a machine. Returns the
 * lower-cased value of `radio-chip` if the user set it, otherwise the
 * provided default ("sx127x" for ESP32 today). Never returns NULL.
 */
static inline const char *esp_radio_chip_or_default(const char *value,
                                                    const char *fallback)
{
    return (value && *value) ? value : fallback;
}

#endif /* HW_MISC_ESP_RADIO_CONFIG_H */
