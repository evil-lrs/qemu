/*
 * Esp Radio Board Config
 *
 * Chip-agnostic representation of a radio board configuration loaded from
 * an ExpressLRS-style flat `hardware.json`. Shared across ESP32, ESP32-S3
 * and ESP32-C3 QEMU machines.
 */

#ifndef HW_MISC_ESP_RADIO_BOARD_H
#define HW_MISC_ESP_RADIO_BOARD_H

#include "qemu/osdep.h"
#include "qapi/error.h"

typedef enum EspRadioType {
    ESP_RADIO_NONE = 0,
    ESP_RADIO_SX127X,
    ESP_RADIO_SX128X,
    ESP_RADIO_LR1121,
} EspRadioType;

typedef struct EspRadioChipConfig {
    int nss;
    int rst;
    int busy;
    int dio0;
    int dio1;
    int dio2;
} EspRadioChipConfig;

#define ESP_BOARD_I2C_MAX 4

typedef struct EspI2cDeviceConfig {
    char type[24];
    int  address;
    /* tmp105 init temperature in millidegrees Celsius; INT_MIN means unset. */
    int  temperature_milli_c;
} EspI2cDeviceConfig;

typedef struct EspRadioBoardConfig {
    EspRadioType type;

    /* SPI controller index the radio is wired to (typically 2=HSPI or
     * 3=VSPI on ESP32). -1 means "not specified"; the machine should treat
     * that as "no radio attached".
     */
    int spi_bus;

    int miso;
    int mosi;
    int sck;

    int chip_count;
    EspRadioChipConfig chips[2];

    bool radio_dcdc;
    bool radio_rfo_hf;
    int  rfsw_ctrl[8];
    int  rfsw_ctrl_len;

    int  i2c_device_count;
    EspI2cDeviceConfig i2c_devices[ESP_BOARD_I2C_MAX];
} EspRadioBoardConfig;

void esp_radio_board_config_init(EspRadioBoardConfig *cfg);

bool esp_radio_board_config_load(const char *path,
                                 EspRadioBoardConfig *cfg,
                                 Error **errp);

const char *esp_radio_type_str(EspRadioType type);

/*
 * Map EspRadioType to its QOM type name (TYPE_SX127X / TYPE_SX128X /
 * TYPE_LR1121). Returns NULL for ESP_RADIO_NONE.
 */
const char *esp_radio_qdev_type(EspRadioType type);

#endif /* HW_MISC_ESP_RADIO_BOARD_H */
