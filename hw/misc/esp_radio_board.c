/*
 * Esp Radio Board Config Parser
 *
 * Reads an ExpressLRS-style flat `hardware.json` and extracts only the
 * radio_* keys needed to wire a QEMU radio mock. All other keys are
 * silently ignored.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qbool.h"
#include "qapi/qmp/qstring.h"
#include "hw/misc/esp_radio_board.h"
#include "hw/ssi/sx127x.h"
#include "hw/ssi/sx128x.h"
#include "hw/ssi/lr1121.h"

void esp_radio_board_config_init(EspRadioBoardConfig *cfg)
{
    g_assert(cfg);
    cfg->type = ESP_RADIO_NONE;
    cfg->miso = -1;
    cfg->mosi = -1;
    cfg->sck  = -1;
    cfg->chip_count = 0;
    for (int i = 0; i < 2; i++) {
        cfg->chips[i].nss  = -1;
        cfg->chips[i].rst  = -1;
        cfg->chips[i].busy = -1;
        cfg->chips[i].dio0 = -1;
        cfg->chips[i].dio1 = -1;
        cfg->chips[i].dio2 = -1;
    }
    cfg->radio_dcdc = false;
    cfg->radio_rfo_hf = false;
    cfg->rfsw_ctrl_len = 0;
    for (int i = 0; i < 8; i++) {
        cfg->rfsw_ctrl[i] = 0;
    }
}

const char *esp_radio_type_str(EspRadioType type)
{
    switch (type) {
    case ESP_RADIO_SX127X: return "sx127x";
    case ESP_RADIO_SX128X: return "sx128x";
    case ESP_RADIO_LR1121: return "lr1121";
    case ESP_RADIO_NONE:
    default:               return "none";
    }
}

const char *esp_radio_qdev_type(EspRadioType type)
{
    switch (type) {
    case ESP_RADIO_SX127X: return TYPE_SX127X;
    case ESP_RADIO_SX128X: return TYPE_SX128X;
    case ESP_RADIO_LR1121: return TYPE_LR1121;
    case ESP_RADIO_NONE:
    default:               return NULL;
    }
}

static bool parse_radio_type(const char *s, EspRadioType *out)
{
    if (!s) {
        return false;
    }
    if (g_ascii_strcasecmp(s, "sx127x") == 0) {
        *out = ESP_RADIO_SX127X;
        return true;
    }
    if (g_ascii_strcasecmp(s, "sx128x") == 0 ||
        g_ascii_strcasecmp(s, "sx1280") == 0 ||
        g_ascii_strcasecmp(s, "sx1281") == 0) {
        *out = ESP_RADIO_SX128X;
        return true;
    }
    if (g_ascii_strcasecmp(s, "lr1121") == 0) {
        *out = ESP_RADIO_LR1121;
        return true;
    }
    return false;
}

/*
 * Pull an integer GPIO pin from the dict, ignoring missing keys.
 * Leaves `*out` unchanged if the key is absent. Returns false if the
 * key exists but is the wrong type — caller decides whether to error.
 */
static bool try_get_pin(const QDict *d, const char *key, int *out)
{
    if (!qdict_haskey(d, key)) {
        return true;
    }
    QObject *obj = qdict_get(d, key);
    QNum *n = qobject_to(QNum, obj);
    if (!n) {
        return false;
    }
    int64_t v;
    if (!qnum_get_try_int(n, &v)) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool try_get_bool(const QDict *d, const char *key, bool *out)
{
    if (!qdict_haskey(d, key)) {
        return true;
    }
    QObject *obj = qdict_get(d, key);
    QBool *b = qobject_to(QBool, obj);
    if (!b) {
        return false;
    }
    *out = qbool_get_bool(b);
    return true;
}

static bool parse_rfsw_ctrl(const QDict *d, EspRadioBoardConfig *cfg,
                            Error **errp)
{
    if (!qdict_haskey(d, "radio_rfsw_ctrl")) {
        return true;
    }
    QObject *obj = qdict_get(d, "radio_rfsw_ctrl");
    QList *list = qobject_to(QList, obj);
    if (!list) {
        error_setg(errp, "radio_rfsw_ctrl must be a JSON array");
        return false;
    }

    int i = 0;
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(list, e) {
        if (i >= 8) {
            break;
        }
        QNum *n = qobject_to(QNum, qlist_entry_obj(e));
        int64_t v;
        if (!n || !qnum_get_try_int(n, &v)) {
            error_setg(errp,
                       "radio_rfsw_ctrl[%d] must be an integer", i);
            return false;
        }
        cfg->rfsw_ctrl[i++] = (int)v;
    }
    cfg->rfsw_ctrl_len = i;
    return true;
}

bool esp_radio_board_config_load(const char *path,
                                 EspRadioBoardConfig *cfg,
                                 Error **errp)
{
    g_assert(path);
    g_assert(cfg);

    esp_radio_board_config_init(cfg);

    gchar *contents = NULL;
    gsize len = 0;
    GError *gerr = NULL;
    if (!g_file_get_contents(path, &contents, &len, &gerr)) {
        error_setg(errp, "radio-config: cannot read '%s': %s",
                   path, gerr ? gerr->message : "unknown error");
        if (gerr) {
            g_error_free(gerr);
        }
        return false;
    }

    QObject *root = qobject_from_json(contents, errp);
    g_free(contents);
    if (!root) {
        return false;
    }

    QDict *d = qobject_to(QDict, root);
    if (!d) {
        error_setg(errp, "radio-config: top-level JSON must be an object");
        qobject_unref(root);
        return false;
    }

    const char *type_str = qdict_get_try_str(d, "radio_type");
    if (!type_str || !parse_radio_type(type_str, &cfg->type)) {
        error_setg(errp,
                   "radio-config: missing or unknown 'radio_type'. "
                   "Expected one of: sx127x, sx128x, lr1121. "
                   "Embedded ExpressLRS hardware.json does not contain "
                   "this key — run tools/extract_radio_config.py to add "
                   "it via binary classification.");
        qobject_unref(root);
        return false;
    }

    bool ok = true;
    ok = ok && try_get_pin(d, "radio_miso", &cfg->miso);
    ok = ok && try_get_pin(d, "radio_mosi", &cfg->mosi);
    ok = ok && try_get_pin(d, "radio_sck",  &cfg->sck);

    ok = ok && try_get_pin(d, "radio_nss",   &cfg->chips[0].nss);
    ok = ok && try_get_pin(d, "radio_rst",   &cfg->chips[0].rst);
    ok = ok && try_get_pin(d, "radio_busy",  &cfg->chips[0].busy);
    ok = ok && try_get_pin(d, "radio_dio0",  &cfg->chips[0].dio0);
    ok = ok && try_get_pin(d, "radio_dio1",  &cfg->chips[0].dio1);

    ok = ok && try_get_pin(d, "radio_nss_2",   &cfg->chips[1].nss);
    ok = ok && try_get_pin(d, "radio_rst_2",   &cfg->chips[1].rst);
    ok = ok && try_get_pin(d, "radio_busy_2",  &cfg->chips[1].busy);
    ok = ok && try_get_pin(d, "radio_dio0_2",  &cfg->chips[1].dio0);
    ok = ok && try_get_pin(d, "radio_dio1_2",  &cfg->chips[1].dio1);

    ok = ok && try_get_bool(d, "radio_dcdc",   &cfg->radio_dcdc);
    ok = ok && try_get_bool(d, "radio_rfo_hf", &cfg->radio_rfo_hf);

    if (!ok) {
        error_setg(errp, "radio-config: a radio_* key has the wrong type");
        qobject_unref(root);
        esp_radio_board_config_init(cfg);
        return false;
    }

    if (!parse_rfsw_ctrl(d, cfg, errp)) {
        qobject_unref(root);
        esp_radio_board_config_init(cfg);
        return false;
    }

    cfg->chip_count = (cfg->chips[1].nss >= 0) ? 2
                    : (cfg->chips[0].nss >= 0) ? 1
                    : 0;

    qobject_unref(root);
    return true;
}
