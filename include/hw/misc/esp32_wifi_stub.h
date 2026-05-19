#pragma once

#include "exec/hwaddr.h"

/*
 * Thin stateful backing for ESP32 peripheral regions that aren't
 * modeled as proper qdev devices.  Each region exposes both a DPORT
 * and an APB-mirror MemoryRegion of `size` bytes.  Reads return the
 * last value written, or `default_val` for offsets never touched.
 *
 * Used for ANA/RTCIO/SENS/IOMUX/FE2/NRX/BB/PHY/PHYB/APBCTRL and the
 * non-PHY I2S/RMT/PCNT/MCPWM regions — anything the firmware pokes
 * during init but where a precise device model isn't needed.  The
 * real Wi-Fi MAC, front-end, and PHY-A are modeled by separate
 * devices vendored from lcgamboa (TYPE_ESP32_WIFI / TYPE_ESP32_FE /
 * TYPE_ESP32_PHYA).
 */
void esp32_wifi_stub_add_region(const char *name, hwaddr dport_base,
                                hwaddr apb_base, size_t size,
                                uint32_t default_val);

/*
 * Same as esp32_wifi_stub_add_region(), but bits set in @self_clear_mask
 * are forced to zero in storage on every write.  Used to model "busy"
 * / "trigger" status bits that the real hardware self-clears once a
 * transaction completes — e.g. NRX bit 25 driven by libphy's
 * i2c_master_reset() loop.  Reads otherwise behave the same as the
 * plain stub: stored value for touched words, @default_val otherwise.
 */
void esp32_wifi_stub_add_region_self_clear(const char *name,
                                           hwaddr dport_base,
                                           hwaddr apb_base, size_t size,
                                           uint32_t default_val,
                                           uint32_t self_clear_mask);
