#include "net.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "net";

#define NET_CMD_Q_LEN 6
#define NET_CMD_LEN   256
#define NET_RETRY_MS  5000

static net_cfg_t s_cfg;
static char      s_topic_cmd[PB_NET_TOPIC_LEN];
static char      s_topic_evt[PB_NET_TOPIC_LEN];
static esp_mqtt_client_handle_t s_mqtt;
static volatile bool s_mqtt_ready;

/* 下行命令环形队列（MQTT 事件任务写，主循环读） */
static char             s_cmd[NET_CMD_Q_LEN][NET_CMD_LEN];
static volatile int     s_cmd_head;
static volatile int     s_cmd_tail;

static void retry_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void ensure_retry_timer(void)
{
    static esp_timer_handle_t s_timer;
    if (s_timer != NULL) {
        esp_timer_stop(s_timer);
        esp_timer_start_once(s_timer, (uint64_t)NET_RETRY_MS * 1000u);
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    if (esp_timer_create(&args, &s_timer) == ESP_OK) {
        esp_timer_start_once(s_timer, (uint64_t)NET_RETRY_MS * 1000u);
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_mqtt_ready = false;
        ESP_LOGW(TAG, "wifi disconnected, retry in %d ms", NET_RETRY_MS);
        ensure_retry_timer();
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&ev->ip_info.ip));

    if (s_mqtt != NULL || s_cfg.uri[0] == '\0') {
        return;
    }
    esp_mqtt_client_config_t mc;
    memset(&mc, 0, sizeof(mc));
    mc.broker.address.uri     = s_cfg.uri;
    mc.credentials.client_id  = s_cfg.device_id;
    mc.session.keepalive      = 30;
    mc.session.last_will.topic  = s_topic_evt;
    mc.session.last_will.msg    = "{\"type\":\"offline\"}";
    mc.session.last_will.qos    = 1;
    mc.session.last_will.retain = 1;

    s_mqtt = esp_mqtt_client_init(&mc);
    if (s_mqtt == NULL) {
        ESP_LOGE(TAG, "mqtt init failed");
        return;
    }
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
}

void net_start(const net_cfg_t *cfg)
{
    if (cfg == NULL) {
        ESP_LOGW(TAG, "no config, network disabled");
        return;
    }
    s_cfg = *cfg;
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "pillbox/%s/cmd", s_cfg.device_id);
    snprintf(s_topic_evt, sizeof(s_topic_evt), "pillbox/%s/event", s_cfg.device_id);

    if (s_cfg.ssid[0] == '\0') {
        ESP_LOGW(TAG, "no wifi config, network disabled");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", s_cfg.ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", s_cfg.pass);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        ip_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi start, ssid=%s mqtt=%s", s_cfg.ssid, s_cfg.uri);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_ready = true;
        esp_mqtt_client_subscribe(s_mqtt, s_topic_cmd, 1);
        ESP_LOGI(TAG, "mqtt up, subscribed %s", s_topic_cmd);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_ready = false;
        ESP_LOGW(TAG, "mqtt down");
        break;
    case MQTT_EVENT_DATA: {
        const size_t tlen = strlen(s_topic_cmd);
        if ((size_t)ev->topic_len != tlen || strncmp(ev->topic, s_topic_cmd, tlen) != 0) {
            break;
        }
        int n = ev->data_len;
        if (n > NET_CMD_LEN - 1) {
            n = NET_CMD_LEN - 1;
        }
        const int next = (s_cmd_tail + 1) % NET_CMD_Q_LEN;
        if (next == s_cmd_head) {
            ESP_LOGW(TAG, "cmd queue full, drop");
            break;
        }
        memcpy(s_cmd[s_cmd_tail], ev->data, (size_t)n);
        s_cmd[s_cmd_tail][n] = '\0';
        s_cmd_tail = next;
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error");
        break;
    default:
        break;
    }
}

bool net_connected(void)
{
    return s_mqtt_ready;
}

pb_err_t net_publish(const char *json)
{
    if (json == NULL) {
        return PB_ERR_INVALID;
    }
    if (!s_mqtt_ready || s_mqtt == NULL) {
        return PB_ERR_IO;
    }
    const int msg_id = esp_mqtt_client_publish(s_mqtt, s_topic_evt, json, 0, 1, 0);
    return (msg_id >= 0) ? PB_OK : PB_ERR_IO;
}

bool net_take_command(char *out, size_t n)
{
    if (out == NULL || n == 0 || s_cmd_head == s_cmd_tail) {
        return false;
    }
    snprintf(out, n, "%s", s_cmd[s_cmd_head]);
    s_cmd_head = (s_cmd_head + 1) % NET_CMD_Q_LEN;
    return true;
}

const char *net_device_id(void)
{
    return s_cfg.device_id;
}
