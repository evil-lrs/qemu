#pragma once

#include "exec/hwaddr.h"
#include "hw/irq.h"

/*
 * Wi-Fi / PHY peripheral stub for the ESP32 machine.
 *
 * The ESP32 model in this QEMU fork has no real Wi-Fi MAC / PHY device.
 * Without help, the IDF's `esp_wifi_init()` / `register_chipv7_phy()`
 * sequence hangs forever polling status bits in regions QEMU declared as
 * `create_unimplemented_device` (returns zero forever).
 *
 * This stub provides two emulation strategies, selectable at runtime via
 * the `wifi-emulation` machine property:
 *
 *   ESP32_WIFI_EMU_DUMMY
 *       Just let the firmware's Wi-Fi init code run to completion.
 *       Writes are echoed on subsequent reads; reads of un-touched
 *       offsets return all-ones (so "wait for ready bit" loops
 *       terminate immediately on the first read).  Auto-detects
 *       busy-poll patterns (>= 16 reads of the same offset with no
 *       intervening write) and latches those offsets to all-ones for
 *       the rest of the run.
 *
 *   ESP32_WIFI_EMU_NETWORK
 *       Reserved for a future network-emulation backend that would
 *       actually move 802.11 frames between QEMU and the host.  Not
 *       implemented yet; currently behaves the same as DUMMY (with a
 *       qemu_log warning so users notice).
 *
 * `esp32_wifi_stub_add_region()` installs one stub region (DPORT view
 * + APB-mirror view, matching the existing
 * `esp32_soc_add_unimp_device()` layout).  The currently-selected mode
 * is held in a single process-global variable set by
 * `esp32_wifi_stub_set_mode()` before any region is installed.
 */

typedef enum Esp32WifiEmuMode {
    ESP32_WIFI_EMU_DUMMY = 0,
    ESP32_WIFI_EMU_NETWORK,
} Esp32WifiEmuMode;

/* Parse a user-supplied string ("dummy"/"network"/NULL). Returns the mode
 * and sets *unknown to true when the string is non-empty but did not
 * match a known value (caller may then error out / log). */
Esp32WifiEmuMode esp32_wifi_stub_parse_mode(const char *s, bool *unknown);

/* Convert mode back to its canonical string name (for logging). */
const char *esp32_wifi_stub_mode_name(Esp32WifiEmuMode mode);

/* Set the mode used by every subsequent `esp32_wifi_stub_add_region()`
 * call.  Must be invoked from the machine's `init` before any region is
 * installed. */
void esp32_wifi_stub_set_mode(Esp32WifiEmuMode mode);

/* Install one stateful stub region at `dport_base` + APB mirror at
 * `apb_base`, both of `size` bytes.  Layered at higher overlap priority
 * than `create_unimplemented_device` so it wins. */
void esp32_wifi_stub_add_region(const char *name, hwaddr dport_base,
                               hwaddr apb_base, size_t size,
                               uint32_t default_val);

/* In `dummy` mode, periodically assert the supplied IRQ lines so any
 * task blocked on a WiFi-MAC / BB level interrupt gets a chance to
 * run.  The IDF Wi-Fi driver's ISR reads the WiFi status register
 * (returned by our stub as `0xffffffff` after the firmware has written
 * to it once) and may post events from there.  No-op when mode is not
 * dummy or when both irqs are NULL.
 *
 * Period is in milliseconds (real time).  Pass `wifi_mac` / `wifi_bb`
 * as qemu_irq lines obtained from the intmatrix
 * (qdev_get_gpio_in(intmatrix, ETS_WIFI_MAC_INTR_SOURCE), etc.). */
void esp32_wifi_stub_start_event_pulser(qemu_irq wifi_mac,
                                        qemu_irq wifi_bb,
                                        unsigned period_ms);
