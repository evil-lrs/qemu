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
#define I2S_C3_RX_CONF_REG_OFF  0x20
#define I2S_C3_TX_CONF_REG_OFF  0x24
#define I2S_C3_STATE_REG_OFF    0x6c
#define I2S_OUT_LINK_REG_OFF    0x34
#define I2S_LC_CONF_REG_OFF     0x60
#define I2S_STATE_REG_OFF       0xbc

#define I2S_CONF_RESET_MASK     0x0000000f
#define I2S_CONF_TX_START       (1u << 4)
#define I2S_C3_CONF_UPDATE      (1u << 8)
#define I2S_C3_TX_START         (1u << 2)
#define I2S_C3_CONF_RESET_MASK  0x00000103
#define I2S_OUT_LINK_START      (1u << 29)
#define I2S_OUT_LINK_RESTART    (1u << 30)
#define I2S_OUT_LINK_STOP       (1u << 28)
#define I2S_LC_CONF_RESET_MASK  0x0000000f
#define I2S_INT_TX_REMPTY       (1u << 5)
#define I2S_INT_OUT_DONE        (1u << 11)
#define I2S_INT_OUT_EOF         (1u << 12)
#define I2S_INT_TX_COMPLETE     (I2S_INT_TX_REMPTY | I2S_INT_OUT_DONE | \
                                  I2S_INT_OUT_EOF)
#define I2S_C3_INT_TX_COMPLETE  (1u << 1)
#define I2S_STATE_IDLE          0x00000007

#define RMT_C3_CH0_CONF0_REG_OFF    0x10
#define RMT_C3_CH1_CONF0_REG_OFF    0x14
#define RMT_C3_INT_RAW_REG_OFF      0x38
#define RMT_C3_INT_ST_REG_OFF       0x3c
#define RMT_C3_INT_ENA_REG_OFF      0x40
#define RMT_C3_INT_CLR_REG_OFF      0x44
#define RMT_C3_TX_START             (1u << 0)
#define RMT_C3_TX_END_MASK          0x00000003

#define SARADC_C3_ONETIME_SAMPLE_REG_OFF 0x20
#define SARADC_C3_DATA_STATUS1_REG_OFF   0x2c
#define SARADC_C3_DATA_STATUS2_REG_OFF   0x30
#define SARADC_C3_INT_ENA_REG_OFF        0x40
#define SARADC_C3_INT_RAW_REG_OFF        0x44
#define SARADC_C3_INT_ST_REG_OFF         0x48
#define SARADC_C3_INT_CLR_REG_OFF        0x4c
#define SARADC_C3_ADC1_ONETIME_SAMPLE    (1u << 31)
#define SARADC_C3_ADC2_ONETIME_SAMPLE    (1u << 30)
#define SARADC_C3_ONETIME_START          (1u << 29)
#define SARADC_C3_ADC1_DONE              (1u << 31)
#define SARADC_C3_ADC2_DONE              (1u << 30)
#define SARADC_C3_DATA_VALUE             1800

typedef struct WifiStubRegion {
    char *name;
    size_t words;
    uint32_t *storage;
    bool *touched;
    uint32_t default_val;
    uint32_t self_clear_mask;
    bool i2s_tx_complete;
    bool c3_i2s;
    bool c3_rmt;
    bool c3_saradc;
    uint32_t rmt_int_raw;
    uint32_t saradc_int_raw;
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
        qemu_set_irq(r->irq, 1);
    }
}

static void wifi_stub_update_rmt_irq(WifiStubRegion *r)
{
    if (r->irq) {
        uint32_t enabled = wifi_stub_raw_read(r, RMT_C3_INT_ENA_REG_OFF);
        qemu_set_irq(r->irq, (r->rmt_int_raw & enabled) ? 1 : 0);
    }
}

static void wifi_stub_raise_rmt_tx_end(WifiStubRegion *r, uint32_t mask)
{
    r->rmt_int_raw |= mask & RMT_C3_TX_END_MASK;
    wifi_stub_update_rmt_irq(r);
}

static void wifi_stub_update_saradc_irq(WifiStubRegion *r)
{
    if (r->irq) {
        uint32_t enabled = wifi_stub_raw_read(r, SARADC_C3_INT_ENA_REG_OFF);
        qemu_set_irq(r->irq, (r->saradc_int_raw & enabled) ? 1 : 0);
    }
}

static void wifi_stub_raise_saradc_done(WifiStubRegion *r, uint32_t sample)
{
    uint32_t mask = 0;

    if (sample & SARADC_C3_ADC1_ONETIME_SAMPLE) {
        mask |= SARADC_C3_ADC1_DONE;
    }
    if (sample & SARADC_C3_ADC2_ONETIME_SAMPLE) {
        mask |= SARADC_C3_ADC2_DONE;
    }
    if (mask == 0) {
        mask = SARADC_C3_ADC1_DONE;
    }

    r->saradc_int_raw |= mask;
    wifi_stub_update_saradc_irq(r);
}

static uint64_t wifi_stub_read(void *opaque, hwaddr off, unsigned size)
{
    WifiStubRegion *r = opaque;
    uint32_t v = wifi_stub_raw_read(r, off);

    if (r->c3_saradc) {
        switch (off) {
        case SARADC_C3_DATA_STATUS1_REG_OFF:
        case SARADC_C3_DATA_STATUS2_REG_OFF:
            v = SARADC_C3_DATA_VALUE;
            break;
        case SARADC_C3_INT_RAW_REG_OFF:
            v = r->saradc_int_raw;
            break;
        case SARADC_C3_INT_ST_REG_OFF:
            v = r->saradc_int_raw &
                wifi_stub_raw_read(r, SARADC_C3_INT_ENA_REG_OFF);
            break;
        default:
            break;
        }
    } else if (r->c3_rmt) {
        switch (off) {
        case RMT_C3_INT_RAW_REG_OFF:
            v = r->rmt_int_raw;
            break;
        case RMT_C3_INT_ST_REG_OFF:
            v = r->rmt_int_raw &
                wifi_stub_raw_read(r, RMT_C3_INT_ENA_REG_OFF);
            break;
        default:
            break;
        }
    } else if (r->irq) {
        uint32_t tx_complete = r->c3_i2s ?
            I2S_C3_INT_TX_COMPLETE : I2S_INT_TX_COMPLETE;
        switch (off) {
        case I2S_INT_RAW_REG_OFF:
            v = r->i2s_tx_complete ? tx_complete : 0;
            break;
        case I2S_INT_ST_REG_OFF:
            v = r->i2s_tx_complete ?
                (tx_complete & wifi_stub_raw_read(r, I2S_INT_ENA_REG_OFF)) : 0;
            break;
        case I2S_C3_STATE_REG_OFF:
        case I2S_STATE_REG_OFF:
            v = I2S_STATE_IDLE;
            break;
        default:
            break;
        }
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
    if (r->c3_saradc) {
        switch (off) {
        case SARADC_C3_ONETIME_SAMPLE_REG_OFF:
            if (val & SARADC_C3_ONETIME_START) {
                wifi_stub_raise_saradc_done(r, val);
            }
            val &= ~SARADC_C3_ONETIME_START;
            break;
        case SARADC_C3_INT_CLR_REG_OFF:
            r->saradc_int_raw &= ~((uint32_t)val);
            wifi_stub_update_saradc_irq(r);
            val = 0;
            break;
        case SARADC_C3_INT_ENA_REG_OFF:
            wifi_stub_update_saradc_irq(r);
            break;
        default:
            break;
        }
    } else if (r->c3_rmt) {
        switch (off) {
        case RMT_C3_CH0_CONF0_REG_OFF:
            if (val & RMT_C3_TX_START) {
                wifi_stub_raise_rmt_tx_end(r, BIT(0));
            }
            val &= ~RMT_C3_TX_START;
            break;
        case RMT_C3_CH1_CONF0_REG_OFF:
            if (val & RMT_C3_TX_START) {
                wifi_stub_raise_rmt_tx_end(r, BIT(1));
            }
            val &= ~RMT_C3_TX_START;
            break;
        case RMT_C3_INT_CLR_REG_OFF:
            r->rmt_int_raw &= ~((uint32_t)val);
            wifi_stub_update_rmt_irq(r);
            val = 0;
            break;
        case RMT_C3_INT_ENA_REG_OFF:
            wifi_stub_update_rmt_irq(r);
            break;
        default:
            break;
        }
    } else if (r->irq) {
        switch (off) {
        case I2S_CONF_REG_OFF:
            if (val & I2S_CONF_TX_START) {
                wifi_stub_raise_i2s_tx_complete(r);
            }
            val &= ~I2S_CONF_RESET_MASK;
            break;
        case I2S_C3_RX_CONF_REG_OFF:
            val &= ~I2S_C3_CONF_RESET_MASK;
            break;
        case I2S_C3_TX_CONF_REG_OFF:
            if (val & I2S_C3_TX_START) {
                wifi_stub_raise_i2s_tx_complete(r);
            }
            val &= ~I2S_C3_CONF_RESET_MASK;
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
    if (r->irq && !r->c3_rmt && off == I2S_INT_ENA_REG_OFF &&
        r->i2s_tx_complete) {
        uint32_t tx_complete = r->c3_i2s ?
            I2S_C3_INT_TX_COMPLETE : I2S_INT_TX_COMPLETE;
        qemu_set_irq(r->irq, (val & tx_complete) ? 1 : 0);
    }
    if (r->c3_rmt && off == RMT_C3_INT_ENA_REG_OFF) {
        wifi_stub_update_rmt_irq(r);
    }
    if (r->c3_saradc && off == SARADC_C3_INT_ENA_REG_OFF) {
        wifi_stub_update_saradc_irq(r);
    }
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

void esp32_wifi_stub_add_region_single(const char *name, hwaddr base,
                                       size_t size, uint32_t default_val)
{
    MemoryRegion *sys_mem = get_system_memory();
    WifiStubRegion *r = g_new0(WifiStubRegion, 1);
    r->name = g_strdup(name);
    r->words = size / 4;
    r->storage = g_new0(uint32_t, r->words);
    r->touched = g_new0(bool, r->words);
    r->default_val = default_val;

    MemoryRegion *mr = g_new0(MemoryRegion, 1);
    memory_region_init_io(mr, NULL, &wifi_stub_ops, r, r->name, size);
    memory_region_add_subregion_overlap(sys_mem, base, mr, 1);
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
    r->c3_i2s = strstr(name, "esp32c3.i2s") != NULL;
    r->c3_rmt = strstr(name, ".rmt") != NULL;
    r->c3_saradc = strstr(name, ".saradc") != NULL;

    MemoryRegion *mr_dport = g_new0(MemoryRegion, 1);
    memory_region_init_io(mr_dport, NULL, &wifi_stub_ops, r, r->name, size);
    memory_region_add_subregion_overlap(sys_mem, dport_base, mr_dport, 0);

    MemoryRegion *mr_apb = g_new0(MemoryRegion, 1);
    char *apb_name = g_strdup_printf("%s-apb", name);
    memory_region_init_io(mr_apb, NULL, &wifi_stub_ops, r, apb_name, size);
    memory_region_add_subregion_overlap(sys_mem, apb_base, mr_apb, 0);
    g_free(apb_name);
}

void esp32_wifi_stub_add_i2s_region_single(const char *name, hwaddr base,
                                           size_t size, uint32_t default_val,
                                           qemu_irq irq)
{
    MemoryRegion *sys_mem = get_system_memory();
    WifiStubRegion *r = g_new0(WifiStubRegion, 1);
    r->name = g_strdup(name);
    r->words = size / 4;
    r->storage = g_new0(uint32_t, r->words);
    r->touched = g_new0(bool, r->words);
    r->default_val = default_val;
    r->irq = irq;
    r->c3_i2s = strstr(name, "esp32c3.i2s") != NULL;
    r->c3_rmt = strstr(name, ".rmt") != NULL;
    r->c3_saradc = strstr(name, ".saradc") != NULL;

    MemoryRegion *mr = g_new0(MemoryRegion, 1);
    memory_region_init_io(mr, NULL, &wifi_stub_ops, r, r->name, size);
    memory_region_add_subregion_overlap(sys_mem, base, mr, 1);
}
