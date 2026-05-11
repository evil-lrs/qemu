#ifndef HW_SSI_SEMTECH_RADIO_COMMON_H
#define HW_SSI_SEMTECH_RADIO_COMMON_H

#include "qemu/osdep.h"
#include "chardev/char-fe.h"

typedef struct SemtechRadioFrame {
    const char *chip;
    const char *radio_id;

    uint64_t freq_hz;
    uint32_t raw_freq;

    const char *packet_type;

    uint8_t sync_word[8];
    size_t sync_word_len;

    uint8_t modem_params[16];
    size_t modem_params_len;

    uint8_t packet_params[16];
    size_t packet_params_len;

    uint8_t payload[256];
    size_t payload_len;

    int rssi_dbm;
    int snr_db;
} SemtechRadioFrame;

void semtech_frame_to_tx_json(const SemtechRadioFrame *f, GString *out);
void semtech_frame_to_state_json(const SemtechRadioFrame *f, bool rx_enabled, GString *out);
void semtech_frame_to_hello_json(const SemtechRadioFrame *f, GString *out);

bool semtech_rx_json_to_frame(const char *line, SemtechRadioFrame *f);

void semtech_frame_log_tx(const SemtechRadioFrame *f, const char *path);

/* Helper for line-buffered RX from chardev */
typedef void (*SemtechRadioRxCallback)(void *opaque, const SemtechRadioFrame *f);

typedef struct SemtechRadioAirBus {
    CharBackend chr;
    bool connected;
    GString *rx_buf;
    SemtechRadioRxCallback rx_cb;
    void *opaque;
} SemtechRadioAirBus;

void semtech_air_bus_init(SemtechRadioAirBus *bus, SemtechRadioRxCallback rx_cb, void *opaque);
void semtech_air_bus_start(SemtechRadioAirBus *bus);
bool semtech_air_bus_send_json(SemtechRadioAirBus *bus, const char *line);

#endif
