/*
 * Thin stateful backing for ESP32 peripheral regions that aren't
 * modeled as proper qdev devices.  See esp32_wifi_stub.h.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "hw/misc/esp32_wifi_stub.h"

typedef struct WifiStubRegion {
    char *name;
    size_t words;
    uint32_t *storage;
    bool *touched;
    uint32_t default_val;
} WifiStubRegion;

static uint64_t wifi_stub_read(void *opaque, hwaddr off, unsigned size)
{
    WifiStubRegion *r = opaque;
    hwaddr idx = off / 4;
    if (idx >= r->words) {
        return r->default_val;
    }
    return r->touched[idx] ? r->storage[idx] : r->default_val;
}

static void wifi_stub_write(void *opaque, hwaddr off, uint64_t val,
                            unsigned size)
{
    WifiStubRegion *r = opaque;
    hwaddr idx = off / 4;
    if (idx >= r->words) {
        return;
    }
    r->storage[idx] = (uint32_t)val;
    r->touched[idx] = true;
}

static const MemoryRegionOps wifi_stub_ops = {
    .read = wifi_stub_read,
    .write = wifi_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void esp32_wifi_stub_add_region(const char *name, hwaddr dport_base,
                                hwaddr apb_base, size_t size,
                                uint32_t default_val)
{
    MemoryRegion *sys_mem = get_system_memory();
    WifiStubRegion *r = g_new0(WifiStubRegion, 1);
    r->name = g_strdup(name);
    r->words = size / 4;
    r->storage = g_new0(uint32_t, r->words);
    r->touched = g_new0(bool, r->words);
    r->default_val = default_val;

    MemoryRegion *mr_dport = g_new0(MemoryRegion, 1);
    memory_region_init_io(mr_dport, NULL, &wifi_stub_ops, r, r->name, size);
    memory_region_add_subregion_overlap(sys_mem, dport_base, mr_dport, 0);

    MemoryRegion *mr_apb = g_new0(MemoryRegion, 1);
    char *apb_name = g_strdup_printf("%s-apb", name);
    memory_region_init_io(mr_apb, NULL, &wifi_stub_ops, r, apb_name, size);
    memory_region_add_subregion_overlap(sys_mem, apb_base, mr_apb, 0);
    g_free(apb_name);
}
