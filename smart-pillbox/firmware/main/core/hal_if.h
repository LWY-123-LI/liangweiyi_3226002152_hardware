/*
 * hal_if.h —— 硬件抽象层（纯 C，不依赖 ESP-IDF，可在 PC 上编译做单元测试）
 *
 * 设计原则：
 *  1) core/ 目录下所有逻辑只依赖本文件，不 include 任何 esp_*.h；
 *  2) 固件侧由 hal_esp.c 用真实驱动实现这些 vtable；
 *  3) 测试侧由 mock_hal.c 实现，可脚本化注入重量/电量/开盖序列。
 */
#ifndef PB_HAL_IF_H
#define PB_HAL_IF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 基础类型 ---------------- */

typedef enum {
    PB_OK = 0,
    PB_ERR_INVALID = -1,
    PB_ERR_TIMEOUT = -2,
    PB_ERR_IO = -3,
    PB_ERR_NOTSUP = -4,
    PB_ERR_NOMEM = -5,
    PB_ERR_BUSY = -6,
} pb_err_t;

#define PB_MAX_SLOTS         8   /* 最多 8 个药格（受 74HC595/165 位数限制） */
#define PB_MAX_DOSES_PER_DAY 4   /* 每格每天最多 4 个服药时刻 */
#define PB_DRUG_NAME_LEN     40  /* UTF-8，含结尾 0 */
#define PB_TEXT_LEN          48

/* ---------------- 语音/手环提示码 ---------------- */

typedef enum {
    PB_PHRASE_DUE          = 1, /* 请按时取药 */
    PB_PHRASE_NOT_FINISHED = 2, /* 药片未取完，请取完 */
    PB_PHRASE_WRONG_SLOT   = 3, /* 请到指定药格取药 */
    PB_PHRASE_MISSED       = 4, /* 该吃药了（超时提醒） */
    PB_PHRASE_LOW_BATTERY  = 5, /* 电量低，请充电 */
    PB_PHRASE_STORAGE      = 6, /* 电量耗尽，已解锁 */
    PB_PHRASE_DOSE_OK      = 7, /* 取药完成 */
    PB_PHRASE_NET_ERR      = 8, /* 网络异常 */
} pb_phrase_t;

typedef enum {
    PB_ALERT_DUE          = 1,
    PB_ALERT_NOT_FINISHED = 2,
    PB_ALERT_MISSED       = 3,
    PB_ALERT_LOW_BATTERY  = 4,
    PB_ALERT_ESCALATE     = 5,
    PB_ALERT_STORAGE      = 6,
    PB_ALERT_OK           = 7,
} pb_alert_t;

/* ---------------- 各硬件能力接口 ---------------- */

/* 称重：read_mg 返回"绝对重量"(毫克，含格子自重)；若驱动已做去皮，则 slot.empty_mg 填 0。
 * 内部需自行做中值/滑动滤波，读数抖动应小于 cfg.noise_mg。 */
typedef struct {
    void      *ctx;
    pb_err_t (*init)   (void *ctx, uint8_t slot);
    pb_err_t (*tare)   (void *ctx, uint8_t slot);            /* 置零（装药前标定空盒） */
    pb_err_t (*read_mg)(void *ctx, uint8_t slot, int32_t *mg);
    pb_err_t (*calibrate)(void *ctx, uint8_t slot, int32_t known_mg); /* 已知砝码标定 */
} pb_weight_ops_t;

/* 电子锁：unlock/lock 为"动作"，get_state 读锁舌到位开关 */
typedef struct {
    void      *ctx;
    pb_err_t (*init)     (void *ctx, uint8_t slot);
    pb_err_t (*lock)     (void *ctx, uint8_t slot);
    pb_err_t (*unlock)   (void *ctx, uint8_t slot);
    pb_err_t (*get_state)(void *ctx, uint8_t slot, bool *locked);
} pb_lock_ops_t;

/* 开盖检测：每格一个霍尔/微动 */
typedef struct {
    void      *ctx;
    pb_err_t (*is_open)(void *ctx, uint8_t slot, bool *open);
} pb_lid_ops_t;

/* 语音播报：phrase 为主语句，text 可为药名（无网络 TTS 时忽略） */
typedef struct {
    void      *ctx;
    pb_err_t (*speak)(void *ctx, uint16_t phrase, const char *text);
    pb_err_t (*stop) (void *ctx);
} pb_audio_ops_t;

/* 显示：UI 由固件侧实现，core 只提需求 */
typedef struct {
    void      *ctx;
    pb_err_t (*show_due)    (void *ctx, uint8_t slot, const char *drug,
                             uint8_t units, uint16_t stock);
    pb_err_t (*show_idle)   (void *ctx, int32_t seconds_to_next, int32_t next_slot);
    pb_err_t (*show_message)(void *ctx, const char *title, const char *line1,
                             const char *line2);
    pb_err_t (*show_storage)(void *ctx);
    pb_err_t (*backlight)   (void *ctx, bool on);
} pb_display_ops_t;

/* 电量 */
typedef struct {
    void      *ctx;
    pb_err_t (*init)    (void *ctx);
    pb_err_t (*read_mv) (void *ctx, uint32_t *mv);
    pb_err_t (*read_soc)(void *ctx, uint8_t *pct);
} pb_power_ops_t;

/* 上行通知：MQTT + BLE 手环 */
typedef struct {
    void      *ctx;
    pb_err_t (*publish_event)  (void *ctx, const char *json);
    pb_err_t (*notify_bracelet)(void *ctx, uint8_t alert, uint8_t slot);
} pb_notify_ops_t;

/* 墙钟（DS3231 + NTP 校时） */
typedef struct {
    void      *ctx;
    pb_err_t (*unix_time)(void *ctx, int64_t *sec);
    bool     (*time_valid)(void *ctx);
} pb_time_ops_t;

typedef struct {
    const pb_weight_ops_t  *weight;
    const pb_lock_ops_t    *lock;
    const pb_lid_ops_t     *lid;
    const pb_audio_ops_t   *audio;
    const pb_display_ops_t *display;
    const pb_power_ops_t   *power;
    const pb_notify_ops_t  *notify;
    const pb_time_ops_t    *time;
} pb_hal_t;

#ifdef __cplusplus
}
#endif

#endif /* PB_HAL_IF_H */
