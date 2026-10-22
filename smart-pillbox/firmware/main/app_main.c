/*
 * app_main.c —— 固件主程序
 *
 * 单线程驱动 core：1Hz 调 pb_core_tick()，同一线程内消费按键与 MQTT 命令，
 * 保证 pillbox.c 的"非线程安全"前提成立（锁任务 / BLE / Wi-Fi 只与本层交互）。
 */
#include "board_pins.h"
#include "hal_esp.h"
#include "pillbox.h"

#include "ble.h"
#include "net.h"
#include "nvs_store.h"
#include "offline_queue.h"
#include "ota.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app";

static pb_core_t *s_core;
static pb_config_t s_cfg;
static pb_slot_t   s_slots[PB_MAX_SLOTS];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------ 参数解析 */

static uint16_t u16_of(const cJSON *o, const char *k, uint16_t def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (uint16_t)v->valueint : def;
}

static int32_t i32_of(const cJSON *o, const char *k, int32_t def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (int32_t)v->valueint : def;
}

static uint32_t u32_of(const cJSON *o, const char *k, uint32_t def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : def;
}

static void cfg_from_json(const cJSON *o, pb_config_t *cfg)
{
    if (!cJSON_IsObject(o)) {
        return;
    }
    cfg->noise_mg             = u32_of(o, "noise_mg", cfg->noise_mg);
    cfg->settle_ms            = u16_of(o, "settle_ms", cfg->settle_ms);
    cfg->verify_timeout_ms    = u16_of(o, "verify_timeout_ms", cfg->verify_timeout_ms);
    cfg->take_window_s        = u16_of(o, "take_window_s", cfg->take_window_s);
    cfg->due_grace_min        = u16_of(o, "due_grace_min", cfg->due_grace_min);
    cfg->remind_interval_s    = u16_of(o, "remind_interval_s", cfg->remind_interval_s);
    cfg->max_retry            = (uint8_t)u16_of(o, "max_retry", cfg->max_retry);
    cfg->battery_warn_pct     = (uint8_t)u16_of(o, "battery_warn_pct", cfg->battery_warn_pct);
    cfg->battery_low_pct      = (uint8_t)u16_of(o, "battery_low_pct", cfg->battery_low_pct);
    cfg->battery_critical_pct = (uint8_t)u16_of(o, "battery_critical_pct", cfg->battery_critical_pct);
    cfg->battery_cutoff_mv    = u32_of(o, "battery_cutoff_mv", cfg->battery_cutoff_mv);
    cfg->tz_offset_s          = i32_of(o, "tz_offset_s", cfg->tz_offset_s);
}

static bool slot_from_json(const cJSON *o, pb_slot_t *s, uint8_t idx)
{
    if (!cJSON_IsObject(o)) {
        return false;
    }
    memset(s, 0, sizeof(*s));
    s->used = true;

    const cJSON *name = cJSON_GetObjectItem(o, "name");
    if (cJSON_IsString(name) && name->valuestring != NULL) {
        snprintf(s->name, sizeof(s->name), "%s", name->valuestring);
    } else {
        snprintf(s->name, sizeof(s->name), "格%u", (unsigned)(idx + 1));
    }

    s->unit_mg           = u16_of(o, "unit_mg", 500);
    s->stock             = u16_of(o, "stock", 0);
    s->empty_mg          = i32_of(o, "empty_mg", 0);
    s->tolerance_pct     = u16_of(o, "tolerance_pct", 15);
    s->allow_residual_mg = i32_of(o, "allow_residual_mg", 0);
    s->take_window_s     = u16_of(o, "take_window_s", 0);

    const cJSON *doses = cJSON_GetObjectItem(o, "doses");
    if (cJSON_IsArray(doses)) {
        const cJSON *d = NULL;
        cJSON_ArrayForEach(d, doses) {
            if (s->dose_n >= PB_MAX_DOSES_PER_DAY) {
                break;
            }
            if (cJSON_IsArray(d)) {
                const cJSON *m = cJSON_GetArrayItem(d, 0);
                const cJSON *u = cJSON_GetArrayItem(d, 1);
                if (cJSON_IsNumber(m) && cJSON_IsNumber(u)) {
                    s->dose[s->dose_n].minute = (uint16_t)m->valueint;
                    s->dose[s->dose_n].units  = (uint8_t)u->valueint;
                    s->dose_n++;
                }
            } else if (cJSON_IsObject(d)) {
                s->dose[s->dose_n].minute = u16_of(d, "minute", 0xFFFF);
                s->dose[s->dose_n].units  = (uint8_t)u16_of(d, "units", 1);
                s->dose_n++;
            }
        }
    }

    /* HH:MM 形式的 doses 也支持：{"at":"08:30","units":1} */
    const cJSON *at = cJSON_GetObjectItem(o, "at");
    if (cJSON_IsString(at) && at->valuestring != NULL && s->dose_n < PB_MAX_DOSES_PER_DAY) {
        int hh = 0, mm = 0;
        if (sscanf(at->valuestring, "%d:%d", &hh, &mm) == 2 && hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
            s->dose[s->dose_n].minute = (uint16_t)(hh * 60 + mm);
            s->dose[s->dose_n].units  = (uint8_t)u16_of(o, "at_units", 1);
            s->dose_n++;
        }
    }
    return true;
}

/* ------------------------------------------------------- 药单下发 / 持久化 */

static uint8_t persist_slots(void)
{
    const uint8_t n = pb_core_slot_count(s_core);
    for (uint8_t i = 0; i < n; ++i) {
        const pb_slot_t *s = pb_core_slot(s_core, i);
        if (s != NULL) {
            s_slots[i] = *s;
        }
    }
    for (uint8_t i = n; i < PB_MAX_SLOTS; ++i) {
        memset(&s_slots[i], 0, sizeof(s_slots[i]));
    }
    store_save(&s_cfg, s_slots, n, pb_core_is_storage_mode(s_core));
    return n;
}

static void plan_from_json(const cJSON *root)
{
    const cJSON *arr = cJSON_GetObjectItem(root, "slots");
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "setplan: no slots array");
        return;
    }

    pb_slot_t tmp[PB_MAX_SLOTS];
    memset(tmp, 0, sizeof(tmp));
    uint8_t n = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (n >= PB_MAX_SLOTS) {
            break;
        }
        if (slot_from_json(it, &tmp[n], n)) {
            n++;
        }
    }
    if (n == 0) {
        ESP_LOGW(TAG, "setplan: empty plan");
        return;
    }

    pb_config_t cfg = s_cfg;
    cfg.slot_count = n;
    cfg_from_json(cJSON_GetObjectItem(root, "cfg"), &cfg);

    /* cfg 在建核时拷贝进 core，因此换药单时重建 core，避免参数不生效 */
    if (pb_core_state(s_core) != PB_ST_IDLE) {
        ESP_LOGW(TAG, "setplan during %s, force rebuild", pb_state_name(pb_core_state(s_core)));
    }
    pb_core_t *nc = pb_core_create(hal_esp_get(), &cfg);
    if (nc == NULL) {
        ESP_LOGE(TAG, "setplan: core alloc failed");
        return;
    }
    const pb_err_t err = pb_core_set_slots(nc, tmp, n);
    if (err != PB_OK) {
        ESP_LOGE(TAG, "setplan rejected by core: err=%d", (int)err);
        pb_core_destroy(nc);
        return;
    }

    pb_core_destroy(s_core);
    s_core = nc;
    s_cfg = cfg;
    hal_esp_set_tz(s_cfg.tz_offset_s);
    persist_slots();
    ESP_LOGI(TAG, "plan applied: %u slots, tz=%ld", (unsigned)n, (long)s_cfg.tz_offset_s);
}

/* ------------------------------------------------------------ 命令分发 */

static void handle_cmd(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "bad json: %s", json);
        return;
    }

    const cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    const char *name = cJSON_IsString(cmd) ? cmd->valuestring : "";
    const int slot = i32_of(root, "slot", -1);
    const uint32_t t = now_ms();

    if (strcmp(name, "unlock") == 0 && slot >= 1 && slot <= PB_MAX_SLOTS) {
        pb_core_cmd_unlock_slot(s_core, (uint8_t)(slot - 1), t);
    } else if (strcmp(name, "unlock_all") == 0) {
        pb_core_cmd_unlock_all(s_core, t);
    } else if (strcmp(name, "ack") == 0) {
        pb_core_cmd_ack(s_core, t);
    } else if (strcmp(name, "storage") == 0) {
        pb_core_cmd_storage(s_core, t);
    } else if (strcmp(name, "silence") == 0) {
        pb_core_cmd_silence(s_core);
    } else if (strcmp(name, "recheck_battery") == 0) {
        pb_core_cmd_recheck_battery(s_core, t);
    } else if (strcmp(name, "settime") == 0) {
        const cJSON *ts = cJSON_GetObjectItem(root, "ts");
        const int64_t secs = cJSON_IsNumber(ts) ? (int64_t)ts->valuedouble : 0;
        if (secs > 1700000000) {
            hal_esp_set_rtc(secs);
        }
    } else if (strcmp(name, "setplan") == 0) {
        plan_from_json(root);
    } else if (strcmp(name, "wifi") == 0) {
        const cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
        const cJSON *pass = cJSON_GetObjectItem(root, "pass");
        if (cJSON_IsString(ssid) && cJSON_IsString(pass)) {
            if (store_set_wifi(ssid->valuestring, pass->valuestring)) {
                ESP_LOGI(TAG, "wifi saved, reboot to apply");
            }
        }
    } else if (strcmp(name, "reboot") == 0) {
        ESP_LOGW(TAG, "reboot requested");
        esp_restart();
    } else {
        ESP_LOGW(TAG, "unknown cmd: %s", name);
    }
    cJSON_Delete(root);
}

/* ------------------------------------------------------------ 按键 / 补发 */

static void keys_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_KEY_OK) | (1ULL << PIN_KEY_SILENCE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static void poll_keys(void)
{
    static bool ok_down;
    static bool silence_down;

    const bool ok = gpio_get_level(PIN_KEY_OK) == 0;
    const bool silence = gpio_get_level(PIN_KEY_SILENCE) == 0;

    if (ok && !ok_down) {
        (void)pb_core_on_button(s_core, 0, now_ms());
    }
    if (silence && !silence_down) {
        (void)pb_core_on_button(s_core, 1, now_ms());
    }
    ok_down = ok;
    silence_down = silence;
}

static void flush_offline(void)
{
    if (!net_connected()) {
        return;
    }
    char item[PB_OQ_ITEM_LEN];
    while (oq_peek(item, sizeof(item))) {
        if (net_publish(item) != PB_OK) {
            break; /* 下一轮再试 */
        }
        oq_drop();
    }
}

/* ---------------------------------------------------------------- 启动 */

void app_main(void)
{
    store_init();
    oq_init();

    ESP_ERROR_CHECK(hal_esp_init());
    keys_init();

    s_cfg = pb_default_config();
    uint8_t n = 0;
    bool latched = false;
    if (store_load(&s_cfg, s_slots, &n, &latched)) {
        ESP_LOGI(TAG, "loaded plan: %u slots%s", (unsigned)n, latched ? " (storage latched)" : "");
    } else {
        ESP_LOGW(TAG, "no plan in NVS, waiting for APP to push setplan");
    }

    hal_esp_set_tz(s_cfg.tz_offset_s);
    s_core = pb_core_create(hal_esp_get(), &s_cfg);
    if (s_core == NULL) {
        ESP_LOGE(TAG, "core alloc failed");
        return;
    }
    if (n > 0) {
        const pb_err_t err = pb_core_set_slots(s_core, s_slots, n);
        if (err != PB_OK) {
            ESP_LOGE(TAG, "stored plan rejected: %d", (int)err);
        }
    }
    if (latched) {
        /* 断电前已因电量过低解锁：恢复为收纳模式，避免重新上锁后打不开 */
        ESP_LOGW(TAG, "restore latched storage mode");
        pb_core_cmd_storage(s_core, now_ms());
    }

    /* 网络与手环：无凭据时自动降级为离线（事件进 NVS 队列） */
    char dev_id[24] = { 0 };
    if (!store_get_device_id(dev_id, sizeof(dev_id)) || dev_id[0] == '\0') {
        snprintf(dev_id, sizeof(dev_id), "SmartPillbox");
    }
    ble_start(dev_id);

    net_cfg_t nc;
    memset(&nc, 0, sizeof(nc));
    (void)store_get_wifi(nc.ssid, sizeof(nc.ssid), nc.pass, sizeof(nc.pass));
    (void)store_get_mqtt(nc.uri, sizeof(nc.uri));
    snprintf(nc.device_id, sizeof(nc.device_id), "%s", dev_id);
    net_start(&nc);

    ESP_LOGI(TAG, "boot done, state=%s", pb_state_name(pb_core_state(s_core)));

    /* 走到这里说明 NVS/硬件/核心都已就绪，可以确认本次 OTA 镜像 */
    ota_log_info();
    ota_boot_mark_valid();

    uint32_t last_tick = 0;
    uint32_t last_persist = now_ms();
    uint32_t last_rtc_sync = now_ms();
    char cmd[PB_OQ_ITEM_LEN];

    for (;;) {
        const uint32_t t = now_ms();

        if ((uint32_t)(t - last_tick) >= 1000u) {
            last_tick = t;
            pb_core_tick(s_core, t);
        }

        poll_keys();

        while (net_take_command(cmd, sizeof(cmd))) {
            handle_cmd(cmd);
        }
        flush_offline();

        /* 每 5 分钟落盘一次（库存/已触发标记），断电不丢当天状态 */
        if ((uint32_t)(t - last_persist) >= 5u * 60u * 1000u) {
            last_persist = t;
            persist_slots();
        }
        /* 每小时用 RTC 重新锚定，抵消内部 RC 计时漂移 */
        if ((uint32_t)(t - last_rtc_sync) >= 60u * 60u * 1000u) {
            last_rtc_sync = t;
            (void)hal_esp_resync_from_rtc();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}


