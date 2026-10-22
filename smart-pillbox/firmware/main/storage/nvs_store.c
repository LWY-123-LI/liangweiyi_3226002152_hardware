#include "nvs_store.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_log.h"
#include "esp_mac.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "store";
#define NS_NAME "pillbox"

typedef struct {
    uint32_t    magic;
    uint16_t    version;
    uint16_t    slot_count;
    uint8_t     storage_latched;
    uint8_t     reserved[3];
    pb_config_t cfg;
    pb_slot_t   slots[PB_MAX_SLOTS];
} store_blob_t;

void store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

bool store_load(pb_config_t *cfg, pb_slot_t *slots, uint8_t *n, bool *storage_latched)
{
    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no namespace, use defaults");
        return false;
    }

    store_blob_t blob;
    size_t len = sizeof(blob);
    const esp_err_t err = nvs_get_blob(h, "plan", &blob, &len);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no saved plan (%s)", esp_err_to_name(err));
        return false;
    }
    if (len != sizeof(blob) || blob.magic != PB_STORE_MAGIC || blob.version != PB_STORE_VERSION) {
        ESP_LOGW(TAG, "plan blob invalid (len=%u, ver=%u)", (unsigned)len, (unsigned)blob.version);
        return false;
    }
    if (blob.slot_count == 0 || blob.slot_count > PB_MAX_SLOTS) {
        return false;
    }

    if (cfg != NULL) {
        *cfg = blob.cfg;
    }
    if (slots != NULL) {
        memcpy(slots, blob.slots, sizeof(blob.slots));
    }
    if (n != NULL) {
        *n = (uint8_t)blob.slot_count;
    }
    if (storage_latched != NULL) {
        *storage_latched = (blob.storage_latched != 0);
    }
    ESP_LOGI(TAG, "plan loaded: %u slots", (unsigned)blob.slot_count);
    return true;
}

bool store_save(const pb_config_t *cfg, const pb_slot_t *slots, uint8_t n, bool storage_latched)
{
    if (cfg == NULL || slots == NULL || n == 0 || n > PB_MAX_SLOTS) {
        return false;
    }

    store_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = PB_STORE_MAGIC;
    blob.version = PB_STORE_VERSION;
    blob.slot_count = n;
    blob.storage_latched = storage_latched ? 1 : 0;
    blob.cfg = *cfg;
    memcpy(blob.slots, slots, sizeof(blob.slots));

    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, "plan", &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save plan failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* ---------------- 字符串配置项 ---------------- */

static bool get_str(const char *key, char *out, size_t n)
{
    if (out == NULL || n == 0) {
        return false;
    }
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = n;
    const esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    out[n - 1] = '\0';
    return true;
}

static bool set_str(const char *key, const char *val)
{
    if (val == NULL) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

bool store_get_wifi(char *ssid, size_t ssid_n, char *pass, size_t pass_n)
{
    return get_str("wifi_ssid", ssid, ssid_n) && get_str("wifi_pass", pass, pass_n);
}

bool store_set_wifi(const char *ssid, const char *pass)
{
    return set_str("wifi_ssid", ssid) && set_str("wifi_pass", pass);
}

bool store_get_mqtt(char *uri, size_t uri_n)
{
    return get_str("mqtt_uri", uri, uri_n);
}

bool store_set_mqtt(const char *uri)
{
    return set_str("mqtt_uri", uri);
}

bool store_get_device_id(char *id, size_t id_n)
{
    if (get_str("dev_id", id, id_n)) {
        return true;
    }
    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return false;
    }
    snprintf(id, id_n, "pb-%02X%02X%02X", mac[3], mac[4], mac[5]);
    (void)set_str("dev_id", id);
    return true;
}
