#include "offline_queue.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "oq";
#define OQ_NS "pb_oq"

typedef struct {
    uint16_t head; /* 下一个读位置 */
    uint16_t tail; /* 下一个写位置 */
    uint16_t count;
    uint16_t reserved;
    char     items[PB_OQ_CAPACITY][PB_OQ_ITEM_LEN];
} oq_blob_t;

static oq_blob_t s_q;
static bool      s_ready;
static bool      s_dirty;

void oq_init(void)
{
    nvs_handle_t h;
    s_ready = false;
    memset(&s_q, 0, sizeof(s_q));

    if (nvs_open(OQ_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "open ns failed, offline queue disabled");
        return;
    }
    size_t len = sizeof(s_q);
    if (nvs_get_blob(h, "q", &s_q, &len) != ESP_OK || len != sizeof(s_q)) {
        memset(&s_q, 0, sizeof(s_q));
    }
    if (s_q.count > PB_OQ_CAPACITY) {
        memset(&s_q, 0, sizeof(s_q));
    }
    nvs_close(h);
    s_ready = true;
    ESP_LOGI(TAG, "offline queue ready, %u pending", (unsigned)s_q.count);
}

static void flush(void)
{
    if (!s_ready || !s_dirty) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(OQ_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, "q", &s_q, sizeof(s_q)) == ESP_OK) {
        (void)nvs_commit(h);
        s_dirty = false;
    }
    nvs_close(h);
}

bool oq_push(const char *json)
{
    if (!s_ready || json == NULL) {
        return false;
    }
    if (s_q.count == PB_OQ_CAPACITY) {
        /* 丢最旧，保留最新（最新的告警更重要） */
        s_q.head = (uint16_t)((s_q.head + 1) % PB_OQ_CAPACITY);
        s_q.count--;
        ESP_LOGW(TAG, "queue full, drop oldest");
    }
    snprintf(s_q.items[s_q.tail], PB_OQ_ITEM_LEN, "%s", json);
    s_q.tail = (uint16_t)((s_q.tail + 1) % PB_OQ_CAPACITY);
    s_q.count++;
    s_dirty = true;
    flush();
    return true;
}

bool oq_peek(char *out, size_t n)
{
    if (!s_ready || out == NULL || n == 0 || s_q.count == 0) {
        return false;
    }
    snprintf(out, n, "%s", s_q.items[s_q.head]);
    return true;
}

void oq_drop(void)
{
    if (!s_ready || s_q.count == 0) {
        return;
    }
    s_q.head = (uint16_t)((s_q.head + 1) % PB_OQ_CAPACITY);
    s_q.count--;
    s_dirty = true;
    flush();
}

uint16_t oq_count(void)
{
    return s_q.count;
}

void oq_clear(void)
{
    memset(&s_q, 0, sizeof(s_q));
    s_dirty = true;
    flush();
}
