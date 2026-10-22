#include "ble.h"

#include "esp_log.h"
#include "host/ble_gatt.h"
#include "host/ble_gatts.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

static const char *TAG = "ble";

#define BLE_SVC_UUID   0xFFF0
#define BLE_CHR_UUID   0xFFF1

static uint8_t  s_own_addr_type;
static uint16_t s_conn       = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_val_handle;
static bool     s_subscribed;

static void advertise(void);
static int  on_gap_event(struct ble_gap_event *event, void *arg);

/* 只读特征（写入请走 MQTT），真正的内容通过 notify 下发 */
static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const uint8_t st = s_subscribed ? 1 : 0;
        return os_mbuf_append(ctxt->om, &st, sizeof(st)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid       = BLE_UUID16_DECLARE(BLE_CHR_UUID),
        .access_cb  = chr_access,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_val_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = BLE_UUID16_DECLARE(BLE_SVC_UUID),
        .characteristics = s_chrs,
    },
    { 0 },
};

static ble_uuid16_t s_adv_uuid[1] = { BLE_UUID16_INIT(BLE_SVC_UUID) };

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16               = s_adv_uuid;
    fields.num_uuids16           = 1;
    fields.uuids16_is_complete    = 1;

    const char *name = ble_svc_gap_device_name();
    fields.name             = (const uint8_t *)name;
    fields.name_len         = (uint8_t)strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, on_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as \"%s\"", name);
    }
}

static int on_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "bracelet connected, handle=%d", s_conn);
        } else {
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "bracelet disconnected, reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_val_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "notify %s", s_subscribed ? "on" : "off");
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr rc=%d", rc);
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_start(const char *device_name)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %d", (int)err);
        return;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(device_name != NULL ? device_name : "SmartPillbox");

    const int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) {
        ble_gatts_add_svcs(s_svcs);
    } else {
        ESP_LOGE(TAG, "gatt cfg rc=%d", rc);
    }

    nimble_port_freertos_init(host_task);
}

bool ble_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

pb_err_t ble_notify(uint8_t alert, uint8_t slot)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return PB_ERR_NOTSUP;
    }
    if (!s_subscribed) {
        return PB_ERR_BUSY;
    }
    const uint8_t payload[2] = { alert, slot };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, sizeof(payload));
    if (om == NULL) {
        return PB_ERR_NOMEM;
    }
    const int rc = ble_gatts_notify_custom(s_conn, s_val_handle, om);
    return (rc == 0) ? PB_OK : PB_ERR_IO;
}

