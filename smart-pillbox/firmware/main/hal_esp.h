/*
 * hal_esp.h —— ESP32-S3 硬件层入口
 * hal_esp.c 实现 hal_if.h 里的全部 vtable，并额外暴露校时/时区接口。
 */
#ifndef PB_HAL_ESP_H
#define PB_HAL_ESP_H

#include "esp_err.h"
#include "hal_if.h"

#include <stdbool.h>
#include <stdint.h>

/* 初始化 GPIO / I2C / LEDC / UART / SPI+LCD，并创建锁与 UI 任务 */
esp_err_t hal_esp_init(void);

/* 取只读 vtable，传给 pb_core_create() */
const pb_hal_t *hal_esp_get(void);

/* 时区偏移（秒），影响 RTC 读写与日程分钟数解释，默认 +8h */
void hal_esp_set_tz(int32_t offset_s);

/* 用 APP/MQTT 下发的 unix 时间同时校准内存时钟与 DS3231 */
bool hal_esp_set_rtc(int64_t unix_s);

/* 从 DS3231 重新锚定内存时钟（建议每小时一次，抵消晶振漂移） */
bool hal_esp_resync_from_rtc(void);

#endif /* PB_HAL_ESP_H */
