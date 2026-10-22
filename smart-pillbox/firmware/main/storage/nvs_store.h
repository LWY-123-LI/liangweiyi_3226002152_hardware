/*
 * nvs_store.h —— 药单/配置/模式持久化
 */
#ifndef PB_NVS_STORE_H
#define PB_NVS_STORE_H

#include "pillbox.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PB_STORE_MAGIC   0x50424F58u /* "PBOX" */
#define PB_STORE_VERSION 1u

void store_init(void);

bool store_load(pb_config_t *cfg, pb_slot_t *slots, uint8_t *n, bool *storage_latched);
bool store_save(const pb_config_t *cfg, const pb_slot_t *slots, uint8_t n, bool storage_latched);

/* 配网信息（首次开机由 APP/BLE 写入） */
bool store_get_wifi(char *ssid, size_t ssid_n, char *pass, size_t pass_n);
bool store_set_wifi(const char *ssid, const char *pass);
bool store_get_mqtt(char *uri, size_t uri_n);
bool store_set_mqtt(const char *uri);
bool store_get_device_id(char *id, size_t id_n);

#endif /* PB_NVS_STORE_H */
