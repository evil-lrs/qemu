#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/ssi/semtech_radio_common.h"
#include "qapi/error.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qstring.h"
#include "qapi/qmp/qbool.h"
#include "qapi/qmp/qnum.h"

static void bytes_to_hex(const uint8_t *src, size_t len, GString *out)
{
    for (size_t i = 0; i < len; i++) {
        g_string_append_printf(out, "%s%02x", i == 0 ? "" : " ", src[i]);
    }
}

static size_t hex_to_bytes(const char *hex, uint8_t *dst, size_t max_len)
{
    size_t count = 0;
    const char *p = hex;
    while (*p && count < max_len) {
        if (isspace(*p)) {
            p++;
            continue;
        }
        if (isxdigit(p[0]) && isxdigit(p[1])) {
            unsigned int val;
            sscanf(p, "%2x", &val);
            dst[count++] = (uint8_t)val;
            p += 2;
        } else {
            break;
        }
    }
    return count;
}

void semtech_frame_to_tx_json(const SemtechRadioFrame *f, GString *out)
{
    g_string_append(out, "{");
    g_string_append_printf(out, "\"type\":\"tx\",");
    g_string_append_printf(out, "\"radio_id\":\"%s\",", f->radio_id ? f->radio_id : "unknown");
    g_string_append_printf(out, "\"chip\":\"%s\",", f->chip ? f->chip : "unknown");
    g_string_append_printf(out, "\"freq_hz\":%" PRIu64 ",", f->freq_hz);
    g_string_append_printf(out, "\"packet_type\":\"%s\",", f->packet_type ? f->packet_type : "unknown");

    g_string_append(out, "\"sync_word\":\"");
    bytes_to_hex(f->sync_word, f->sync_word_len, out);
    g_string_append(out, "\",");

    g_string_append(out, "\"payload_hex\":\"");
    bytes_to_hex(f->payload, f->payload_len, out);
    g_string_append(out, "\"");

    g_string_append(out, "}\n");
}

void semtech_frame_to_state_json(const SemtechRadioFrame *f, bool rx_enabled, GString *out)
{
    g_string_append(out, "{");
    g_string_append_printf(out, "\"type\":\"state\",");
    g_string_append_printf(out, "\"radio_id\":\"%s\",", f->radio_id ? f->radio_id : "unknown");
    g_string_append_printf(out, "\"rx_enabled\":%s,", rx_enabled ? "true" : "false");
    g_string_append_printf(out, "\"freq_hz\":%" PRIu64 ",", f->freq_hz);
    g_string_append_printf(out, "\"packet_type\":\"%s\",", f->packet_type ? f->packet_type : "unknown");

    g_string_append(out, "\"sync_word\":\"");
    bytes_to_hex(f->sync_word, f->sync_word_len, out);
    g_string_append(out, "\"");

    g_string_append(out, "}\n");
}

void semtech_frame_to_hello_json(const SemtechRadioFrame *f, GString *out)
{
    g_string_append(out, "{");
    g_string_append_printf(out, "\"type\":\"hello\",");
    g_string_append_printf(out, "\"radio_id\":\"%s\",", f->radio_id ? f->radio_id : "unknown");
    g_string_append_printf(out, "\"chip\":\"%s\"", f->chip ? f->chip : "unknown");
    g_string_append(out, "}\n");
}

bool semtech_rx_json_to_frame(const char *line, SemtechRadioFrame *f)
{
    QObject *obj = qobject_from_json(line, NULL);
    if (!obj) {
        return false;
    }

    QDict *dict = qobject_to(QDict, obj);
    if (!dict) {
        qobject_unref(obj);
        return false;
    }

    const char *type = qdict_get_try_str(dict, "type");
    if (!type || strcmp(type, "rx") != 0) {
        qobject_unref(obj);
        return false;
    }

    const char *payload_hex = qdict_get_try_str(dict, "payload_hex");
    if (payload_hex) {
        f->payload_len = hex_to_bytes(payload_hex, f->payload, sizeof(f->payload));
    } else {
        f->payload_len = 0;
    }

    f->rssi_dbm = qdict_get_try_int(dict, "rssi", -100);
    f->snr_db = qdict_get_try_int(dict, "snr", 10);

    qobject_unref(obj);
    return true;
}

void semtech_frame_log_tx(const SemtechRadioFrame *f, const char *path)
{
    if (!path) {
        path = "/tmp/qemu-semtech-radio-tx.jsonl";
    }

    GString *out = g_string_new("");
    semtech_frame_to_tx_json(f, out);

    FILE *fp = fopen(path, "a");
    if (fp) {
        fputs(out->str, fp);
        fclose(fp);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "SemtechRadio: failed to open TX log %s\n", path);
    }

    g_string_free(out, true);
}

static int radio_chr_can_read(void *opaque)
{
    return 1024;
}

static void radio_chr_read(void *opaque, const uint8_t *buf, int size)
{
    SemtechRadioAirBus *bus = opaque;
    g_string_append_len(bus->rx_buf, (const char *)buf, size);

    char *nl;
    while ((nl = strchr(bus->rx_buf->str, '\n'))) {
        *nl = '\0';
        SemtechRadioFrame f = {0};
        if (semtech_rx_json_to_frame(bus->rx_buf->str, &f)) {
            if (bus->rx_cb) {
                bus->rx_cb(bus->opaque, &f);
            }
        }
        g_string_erase(bus->rx_buf, 0, nl - bus->rx_buf->str + 1);
    }
}

static void radio_chr_event(void *opaque, QEMUChrEvent event)
{
    SemtechRadioAirBus *bus = opaque;
    switch (event) {
    case CHR_EVENT_OPENED:
        bus->connected = true;
        qemu_log("SemtechRadio: air-bus connected\n");
        /*
         * Note: we can't send hello/state here easily because we don't
         * have the radio state. The radio model should call
         * semtech_air_bus_send_json manually after connection or in its
         * own handler.
         */
        break;
    case CHR_EVENT_CLOSED:
        bus->connected = false;
        qemu_log("SemtechRadio: air-bus disconnected\n");
        break;
    default:
        break;
    }
}

void semtech_air_bus_init(SemtechRadioAirBus *bus, SemtechRadioRxCallback rx_cb, void *opaque)
{
    bus->connected = false;
    bus->rx_buf = g_string_new("");
    bus->rx_cb = rx_cb;
    bus->opaque = opaque;
}

void semtech_air_bus_start(SemtechRadioAirBus *bus)
{
    /*
     * We assume bus->chr is already initialized (e.g. via DEFINE_PROP_CHR).
     * We just set handlers.
     */
    qemu_chr_fe_set_handlers(&bus->chr,
                             radio_chr_can_read,
                             radio_chr_read,
                             radio_chr_event,
                             NULL,
                             bus,
                             NULL,
                             true);
    if (qemu_chr_fe_backend_connected(&bus->chr)) {
        bus->connected = true;
    }
}

bool semtech_air_bus_send_json(SemtechRadioAirBus *bus, const char *line)
{
    if (!bus->connected) {
        return false;
    }

    int len = strlen(line);
    int written = qemu_chr_fe_write(&bus->chr, (const uint8_t *)line, len);
    return written == len;
}
