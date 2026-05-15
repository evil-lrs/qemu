/*
 * ESP32 Wi-Fi / PHY peripheral stub.
 *
 * Replaces `create_unimplemented_device` placeholders for the BB, FE,
 * ANALOG, RTCIO, SENS, IOMUX, and WiFi-MAC regions with a stateful
 * memory backing that satisfies the IDF's PHY/BB/FE calibration
 * handshake during `esp_wifi_init()`.
 *
 * Two strategies are supported via the `wifi-emulation` machine
 * property (see esp32_wifi_stub.h).  Today both DUMMY and NETWORK
 * behave identically; NETWORK is reserved for a future real-traffic
 * backend and currently just logs a warning.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "hw/misc/esp32_wifi_stub.h"

typedef struct WifiStubOverlay {
    hwaddr offset;
    uint32_t value;
} WifiStubOverlay;

typedef struct WifiStubRegion {
    char *name;
    size_t size;
    uint32_t *storage;
    bool *touched;
    /* Per-word read counter since the last write; used to detect
     * "busy poll waiting for a status bit to flip" loops. */
    uint32_t *spin_reads;
    /* Per-word: latched to true once we've decided this offset is
     * being polled for a status bit.  Sticky: writes do NOT reset it
     * (IDF PHY calibration loops alternate writes to control
     * registers with reads of the *same* status register and would
     * otherwise re-poll for ever after every write). */
    bool *poll_unlock;
    const WifiStubOverlay *overlays;
    size_t overlay_count;
    Esp32WifiEmuMode mode;
    uint32_t default_val;
} WifiStubRegion;

#define WIFI_STUB_POLL_UNLOCK_THRESHOLD 16

/* Process-global mode set by `esp32_wifi_stub_set_mode()` before any
 * region is added.  Each region snapshots the value at install time. */
static Esp32WifiEmuMode g_wifi_stub_mode = ESP32_WIFI_EMU_DUMMY;

/* ---------- per-region overlay tables ---------- */

static const WifiStubOverlay overlays_bb[] = { };
static const WifiStubOverlay overlays_fe[] = { };
static const WifiStubOverlay overlays_fe2[] = { };
static const WifiStubOverlay overlays_rtcio[] = { };
static const WifiStubOverlay overlays_sens[] = { };
static const WifiStubOverlay overlays_iomux[] = { };
static const WifiStubOverlay overlays_nrx[] = {
    /* PHY `set_chan_reg` loops while bit 25 of 0x3ff4e000 is SET.
     * Return 0 to satisfy the "wait until bit cleared" condition. */
    { 0x0000, 0x00000000 },
};
static const WifiStubOverlay overlays_apbctrl[] = {
    { 0x0000, 0x00000001 },              /* SYSCLK_CONF: pre_div_cnt=1 */
    { 0x0004, 0x00000000 },              /* TICK_CONF */
    { 0x0078, 0x51454d55 },              /* QEMU marker: "QEMU" */
    { 0x007c, 0x16042000 | 0x80000000 }, /* ECO3 silicon revision */
};

static const WifiStubOverlay overlays_analog[] = {
    /* RTC_I2C interrupt-status register: every IDF
     * `phy_i2c_master_read/write` issues a command at +0x10/+0x14 and
     * busy-polls +0x4c for the transaction-done bit.  With no real
     * I2C bus underneath, return all-ones so the wait loop terminates
     * on the first read. */
    { 0x004c, 0xffffffff },
    /* SAR ADC / RTC analog status register polled by phy_init while
     * tuning the bias voltage. */
    { 0x00c4, 0xffffffff },
};

static const WifiStubOverlay *lookup_overlay(const char *name,
                                             size_t *count_out)
{
    if (g_str_equal(name, "esp32.bb"))      { *count_out = G_N_ELEMENTS(overlays_bb);      return overlays_bb;      }
    if (g_str_equal(name, "esp32.fe"))      { *count_out = G_N_ELEMENTS(overlays_fe);      return overlays_fe;      }
    if (g_str_equal(name, "esp32.fe2"))     { *count_out = G_N_ELEMENTS(overlays_fe2);     return overlays_fe2;     }
    if (g_str_equal(name, "esp32.analog"))  { *count_out = G_N_ELEMENTS(overlays_analog);  return overlays_analog;  }
    if (g_str_equal(name, "esp32.rtcio"))   { *count_out = G_N_ELEMENTS(overlays_rtcio);   return overlays_rtcio;   }
    if (g_str_equal(name, "esp32.sens"))    { *count_out = G_N_ELEMENTS(overlays_sens);    return overlays_sens;    }
    if (g_str_equal(name, "esp32.iomux"))   { *count_out = G_N_ELEMENTS(overlays_iomux);   return overlays_iomux;   }
    if (g_str_equal(name, "esp32.nrx"))     { *count_out = G_N_ELEMENTS(overlays_nrx);     return overlays_nrx;     }
    if (g_str_equal(name, "esp32.apbctrl")) { *count_out = G_N_ELEMENTS(overlays_apbctrl); return overlays_apbctrl; }
    *count_out = 0;
    return NULL;
}

static bool overlay_lookup(WifiStubRegion *r, hwaddr offset, uint32_t *out)
{
    for (size_t i = 0; i < r->overlay_count; i++) {
        if (r->overlays[i].offset == (offset & ~3ULL)) {
            *out = r->overlays[i].value;
            return true;
        }
    }
    return false;
}

/* ---------- MMIO read / write ops ---------- */

static uint64_t wifi_stub_read(void *opaque, hwaddr offset, unsigned size)
{
    WifiStubRegion *r = opaque;
    size_t word = offset >> 2;
    uint32_t word_val = 0;
    bool from_overlay = overlay_lookup(r, offset, &word_val);
    bool quiet = false; /* from_overlay; */

    if (!from_overlay && word < (r->size >> 2)) {
        if (r->poll_unlock[word]) {
            word_val = 0xffffffffu;
            quiet = true;
        } else if (r->touched[word]) {
            /* Echo previously-written value. */
            word_val = r->storage[word];
            if (++r->spin_reads[word] >= WIFI_STUB_POLL_UNLOCK_THRESHOLD) {
                r->poll_unlock[word] = true;
                word_val = 0xffffffffu;
                quiet = true;
                qemu_log_mask(LOG_UNIMP,
                              "%s: stub poll-unlock at offset=0x%04"
                              HWADDR_PRIx " (latched to 0xffffffff)\n",
                              r->name, offset & ~3ULL);
            }
        } else {
            /* Never written.  In dummy mode return default_val so any
             * "wait until bit X is set" loop terminates on first read.
             * The risk of returning default_val for a device-ID-style
             * register that real silicon would zero is accepted: the
             * regions we stub here have no other behaviour in this
             * fork, and IDF's PHY init treats every status bit it
             * polls as "ready when set" (if default is 0xffffffff)
             * or "ready when clear" (if default is 0). */
            word_val = r->default_val;
            quiet = true;
        }
    }

    uint64_t val;
    if (size == 4) {
        val = word_val;
    } else {
        unsigned shift = (offset & 3) << 3;
        val = (word_val >> shift) & ((1ULL << (size << 3)) - 1);
    }

    if (!quiet) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: stub read  size=%u offset=0x%04" HWADDR_PRIx
                      " -> 0x%" PRIx64 "\n",
                      r->name, size, offset, val);
    }
    return val;
}

static void wifi_stub_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    WifiStubRegion *r = opaque;
    size_t word = offset >> 2;
    if (word >= (r->size >> 2)) {
        return;
    }

    uint32_t cur = r->storage[word];
    if (size == 4) {
        cur = (uint32_t)value;
    } else {
        unsigned shift = (offset & 3) << 3;
        uint32_t mask = ((1ULL << (size << 3)) - 1) << shift;
        cur = (cur & ~mask) | (((uint32_t)value << shift) & mask);
    }
    r->storage[word] = cur;
    r->touched[word] = true;
    /* Writes reset the spin counter so a *fresh* polling cycle has to
     * re-cross the threshold.  poll_unlock is sticky on purpose: once
     * we decide an offset is a status register, keep it latched. */
    r->spin_reads[word] = 0;

    qemu_log_mask(LOG_UNIMP,
                  "%s: stub write size=%u offset=0x%04" HWADDR_PRIx
                  " value=0x%" PRIx64 "\n",
                  r->name, size, offset, value);
}

static const MemoryRegionOps wifi_stub_ops = {
    .read = wifi_stub_read,
    .write = wifi_stub_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/* ---------- public API ---------- */

Esp32WifiEmuMode esp32_wifi_stub_parse_mode(const char *s, bool *unknown)
{
    if (unknown) {
        *unknown = false;
    }
    if (!s || !*s) {
        return ESP32_WIFI_EMU_DUMMY;
    }
    if (g_ascii_strcasecmp(s, "dummy") == 0) {
        return ESP32_WIFI_EMU_DUMMY;
    }
    if (g_ascii_strcasecmp(s, "network") == 0) {
        return ESP32_WIFI_EMU_NETWORK;
    }
    if (unknown) {
        *unknown = true;
    }
    return ESP32_WIFI_EMU_DUMMY;
}

const char *esp32_wifi_stub_mode_name(Esp32WifiEmuMode mode)
{
    switch (mode) {
    case ESP32_WIFI_EMU_DUMMY:   return "dummy";
    case ESP32_WIFI_EMU_NETWORK: return "network";
    }
    return "?";
}

void esp32_wifi_stub_set_mode(Esp32WifiEmuMode mode)
{
    g_wifi_stub_mode = mode;
    if (mode == ESP32_WIFI_EMU_NETWORK) {
        qemu_log("ESP32 wifi-emulation: network mode requested; "
                 "no real network backend is implemented yet, "
                 "falling back to dummy semantics.\n");
    } else {
        qemu_log("ESP32 wifi-emulation: %s\n",
                 esp32_wifi_stub_mode_name(mode));
    }
}

/* ---------- IRQ pulser (dummy-mode helper) ---------- */
/*
 * In dummy mode the firmware can end up sitting in FreeRTOS idle after
 * `esp_wifi_init()` because the WiFi MAC driver registers a level
 * interrupt handler that, on real silicon, would fire from the MAC
 * peripheral.  With no peripheral the handler never runs, no event
 * gets posted, and any task blocked on a Wi-Fi-event group sleeps for
 * ever.
 *
 * The pulser fires the WiFi-MAC (source 0) and WiFi-BB (source 2) IRQ
 * lines briefly at a configurable cadence so the IDF ISR runs.  The
 * ISR reads the stubbed status register (which returns all-ones via
 * our default-unread overlay), so even on a "spurious" trigger it has
 * a path to either consume the interrupt or post an event.  Worst
 * case: handler exits silently and we try again next period.
 */
typedef struct WifiStubPulser {
    qemu_irq mac;
    qemu_irq bb;
    QEMUTimer *timer;
    unsigned period_ms;
    bool phase_high;
} WifiStubPulser;

static WifiStubPulser g_wifi_pulser;

static void wifi_stub_pulser_tick(void *opaque)
{
    WifiStubPulser *p = opaque;

    /* Level-sensitive sources expect a high→low transition to deassert
     * after the handler ran.  Alternate phases per tick. */
    if (!p->phase_high) {
        if (p->mac) qemu_irq_raise(p->mac);
        if (p->bb)  qemu_irq_raise(p->bb);
    } else {
        if (p->mac) qemu_irq_lower(p->mac);
        if (p->bb)  qemu_irq_lower(p->bb);
    }
    p->phase_high = !p->phase_high;

    int64_t next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                 + (int64_t)p->period_ms * 1000000LL;
    timer_mod(p->timer, next);
}

void esp32_wifi_stub_start_event_pulser(qemu_irq wifi_mac,
                                        qemu_irq wifi_bb,
                                        unsigned period_ms)
{
    if (g_wifi_stub_mode != ESP32_WIFI_EMU_DUMMY) {
        return;
    }
    if (!wifi_mac && !wifi_bb) {
        return;
    }
    if (period_ms == 0) {
        period_ms = 100;
    }
    g_wifi_pulser.mac = wifi_mac;
    g_wifi_pulser.bb  = wifi_bb;
    g_wifi_pulser.period_ms = period_ms;
    g_wifi_pulser.phase_high = false;
    g_wifi_pulser.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       wifi_stub_pulser_tick,
                                       &g_wifi_pulser);
    /* First tick after `period_ms` so we don't fire before the firmware
     * has had a chance to register its handler. */
    int64_t first = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                  + (int64_t)period_ms * 1000000LL;
    timer_mod(g_wifi_pulser.timer, first);
    qemu_log("ESP32 wifi-emulation: event pulser armed "
             "(mac=%p bb=%p period=%ums)\n",
             (void *)wifi_mac, (void *)wifi_bb, period_ms);
}

void esp32_wifi_stub_add_region(const char *name, hwaddr dport_base,
                               hwaddr apb_base, size_t size,
                               uint32_t default_val)
{
    MemoryRegion *sys = get_system_memory();

    WifiStubRegion *r = g_new0(WifiStubRegion, 1);
    r->name = g_strdup(name);
    r->size = size;
    r->storage = g_new0(uint32_t, size >> 2);
    r->touched = g_new0(bool, size >> 2);
    r->spin_reads = g_new0(uint32_t, size >> 2);
    r->poll_unlock = g_new0(bool, size >> 2);
    r->overlays = lookup_overlay(r->name, &r->overlay_count);
    r->mode = g_wifi_stub_mode;
    r->default_val = default_val;
    MemoryRegion *dport_mr = g_new(MemoryRegion, 1);
    memory_region_init_io(dport_mr, NULL, &wifi_stub_ops, r, r->name, size);
    memory_region_add_subregion_overlap(sys, dport_base, dport_mr, 100);

    MemoryRegion *apb_mr = g_new(MemoryRegion, 1);
    char *apb_name = g_strdup_printf("%s-apb", name);
    memory_region_init_io(apb_mr, NULL, &wifi_stub_ops, r, apb_name, size);
    memory_region_add_subregion_overlap(sys, apb_base, apb_mr, 100);
    g_free(apb_name);
}
