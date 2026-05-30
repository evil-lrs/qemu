/*
 * Thin stateful backing for ESP32 peripheral regions that aren't
 * modeled as proper qdev devices.  See esp32_wifi_stub.h.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "hw/misc/esp32_wifi_stub.h"
#include "hw/irq.h"

#define I2S_CONF_REG_OFF        0x08
#define I2S_INT_RAW_REG_OFF     0x0c
#define I2S_INT_ST_REG_OFF      0x10
#define I2S_INT_ENA_REG_OFF     0x14
#define I2S_INT_CLR_REG_OFF     0x18
#define I2S_OUT_LINK_REG_OFF    0x34
#define I2S_LC_CONF_REG_OFF     0x60
#define I2S_STATE_REG_OFF       0xbc

#define I2S_CONF_RESET_MASK     0x0000000f
#define I2S_CONF_TX_START       (1u << 4)
#define I2S_OUT_LINK_START      (1u << 29)
#define I2S_OUT_LINK_RESTART    (1u << 30)
#define I2S_OUT_LINK_STOP       (1u << 28)
#define I2S_LC_CONF_RESET_MASK  0x0000000f
#define I2S_INT_TX_REMPTY       (1u << 5)
#define I2S_INT_OUT_DONE        (1u << 11)
#define I2S_INT_OUT_EOF         (1u << 12)
#define I2S_INT_TX_COMPLETE     (I2S_INT_TX_REMPTY | I2S_INT_OUT_DONE | \
                                  I2S_INT_OUT_EOF)
#define I2S_STATE_IDLE          0x00000007

typedef struct WifiStubRegion {
    char *name;
    size_t words;
    uint32_t *storage;
    bool *touched;
    uint32_t default_val;
    uint32_t self_clear_mask;
    bool i2s_tx_complete;
    qemu_irq irq;
} WifiStubRegion;

static uint32_t wifi_stub_raw_read(WifiStubRegion *r, hwaddr off)
{
    hwaddr idx = off / 4;

    if (idx >= r->words) {
        return r->default_val;
    }
    return r->touched[idx] ? r->storage[idx] : r->default_val;
}

static void wifi_stub_raise_i2s_tx_complete(WifiStubRegion *r)
{
    r->i2s_tx_complete = true;
    if (r->irq) {
        qemu_irq_pulse(r->irq);
    }
}

static uint64_t wifi_stub_read(void *opaque, hwaddr off, unsigned size)
{
    WifiStubRegion *r = opaque;
    uint32_t v = wifi_stub_raw_read(r, off);

    switch (off) {
    case I2S_INT_RAW_REG_OFF:
        if (r->irq) {
            v = r->i2s_tx_complete ? I2S_INT_TX_COMPLETE : 0;
        }
        break;
    case I2S_INT_ST_REG_OFF:
        if (r->irq) {
            v = r->i2s_tx_complete ?
                (I2S_INT_TX_COMPLETE &
                 wifi_stub_raw_read(r, I2S_INT_ENA_REG_OFF)) : 0;
        }
        break;
    case I2S_STATE_REG_OFF:
        if (r->irq) {
            v = I2S_STATE_IDLE;
        }
        break;
    default:
        break;
    }

    /* Bits in self_clear_mask model hardware "busy" / "trigger"
     * flags that the real silicon auto-clears once a transaction
     * completes.  Always report them as 0 so polling loops
     * (e.g. libphy's i2c_master_reset) terminate. */
    return v & ~r->self_clear_mask;
}

static void wifi_stub_write(void *opaque, hwaddr off, uint64_t val,
                            unsigned size)
{
    WifiStubRegion *r = opaque;
    hwaddr idx = off / 4;
    if (idx >= r->words) {
        return;
    }
    if (r->irq) {
        switch (off) {
        case I2S_CONF_REG_OFF:
            if (val & I2S_CONF_TX_START) {
                wifi_stub_raise_i2s_tx_complete(r);
            }
            val &= ~I2S_CONF_RESET_MASK;
            break;
        case I2S_OUT_LINK_REG_OFF:
            if (val & (I2S_OUT_LINK_START | I2S_OUT_LINK_RESTART)) {
                wifi_stub_raise_i2s_tx_complete(r);
            }
            val &= ~(I2S_OUT_LINK_START | I2S_OUT_LINK_RESTART |
                     I2S_OUT_LINK_STOP);
            break;
        case I2S_LC_CONF_REG_OFF:
            val &= ~I2S_LC_CONF_RESET_MASK;
            break;
        case I2S_INT_CLR_REG_OFF:
            r->i2s_tx_complete = false;
            qemu_set_irq(r->irq, 0);
            break;
        default:
            break;
        }
    }
    r->storage[idx] = ((uint32_t)val) & ~r->self_clear_mask;
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

static void esp32_wifi_stub_add_region_internal(const char *name,
                                                hwaddr dport_base,
                                                hwaddr apb_base, size_t size,
                                                uint32_t default_val,
                                                uint32_t self_clear_mask)
{
    MemoryRegion *sys_mem = get_system_memory();
    WifiStubRegion *r = g_new0(WifiStubRegion, 1);
    r->name = g_strdup(name);
    r->words = size / 4;
    r->storage = g_new0(uint32_t, r->words);
    r->touched = g_new0(bool, r->words);
    r->default_val = default_val;
    r->self_clear_mask = self_clear_mask;

    MemoryRegion *mr_dport = g_new0(MemoryRegion, 1);
    memory_region_init_io(mr_dport, NULL, &wifi_stub_ops, r, r->name, size);
    memory_region_add_subregion_overlap(sys_mem, dport_base, mr_dport, 0);

    MemoryRegion *mr_apb = g_new0(MemoryRegion, 1);
    char *apb_name = g_strdup_printf("%s-apb", name);
    memory_region_init_io(mr_apb, NULL, &wifi_stub_ops, r, apb_name, size);
    memory_region_add_subregion_overlap(sys_mem, apb_base, mr_apb, 0);
    g_free(apb_name);
}

void esp32_wifi_stub_add_region(const char *name, hwaddr dport_base,
                                hwaddr apb_base, size_t size,
                                uint32_t default_val)
{
    esp32_wifi_stub_add_region_internal(name, dport_base, apb_base, size,
                                        default_val, 0);
}

void esp32_wifi_stub_add_region_self_clear(const char *name,
                                           hwaddr dport_base,
                                           hwaddr apb_base, size_t size,
                                           uint32_t default_val,
                                           uint32_t self_clear_mask)
{
    esp32_wifi_stub_add_region_internal(name, dport_base, apb_base, size,
                                        default_val, self_clear_mask);
}

void esp32_wifi_stub_add_i2s_region(const char *name, hwaddr dport_base,
                                    hwaddr apb_base, size_t size,
                                    uint32_t default_val, qemu_irq irq)
{
    MemoryRegion *sys_mem = get_system_memory();
    WifiStubRegion *r = g_new0(WifiStubRegion, 1);
    r->name = g_strdup(name);
    r->words = size / 4;
    r->storage = g_new0(uint32_t, r->words);
    r->touched = g_new0(bool, r->words);
    r->default_val = default_val;
    r->irq = irq;

    MemoryRegion *mr_dport = g_new0(MemoryRegion, 1);
    memory_region_init_io(mr_dport, NULL, &wifi_stub_ops, r, r->name, size);
    memory_region_add_subregion_overlap(sys_mem, dport_base, mr_dport, 0);

    MemoryRegion *mr_apb = g_new0(MemoryRegion, 1);
    char *apb_name = g_strdup_printf("%s-apb", name);
    memory_region_init_io(mr_apb, NULL, &wifi_stub_ops, r, apb_name, size);
    memory_region_add_subregion_overlap(sys_mem, apb_base, mr_apb, 0);
    g_free(apb_name);
}
