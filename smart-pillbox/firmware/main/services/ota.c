#include "ota.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "ota";

void ota_log_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "fw %s (built %s %s), partition %s",
             app->version, app->date, app->time,
             run != NULL ? run->label : "?");
}

void ota_boot_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run == NULL || run->type != ESP_PARTITION_TYPE_APP) {
        return;
    }
    if (run->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
        run->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1) {
        return; /* factory 分区没有回滚语义 */
    }

    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) {
        return;
    }
    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        return; /* 已确认过，或分区已标记无效 */
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "ota image confirmed: %s", run->label);
    } else {
        ESP_LOGE(TAG, "ota confirm failed");
    }
}
