/*
 * pillbox.h —— 取药会话核心（纯 C，可在 PC 上跑）
 *
 * 状态机：
 *   INIT → IDLE ⇄ (DUE → WAIT_TAKE → TAKING → VERIFY) → MISSED / PARTIAL / ESCALATE
 *           └──────────── 任意状态下电量临界 → STORAGE（全部解锁，变普通收纳盒）
 */
#ifndef PB_PILLBOX_H
#define PB_PILLBOX_H

#include "hal_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一个服药时刻 */
typedef struct {
    uint16_t minute; /* 本地时间 0..1439（APP 下发 HH:MM 转换而来） */
    uint8_t  units;  /* 本次服用片数 */
} pb_dose_point_t;

/* 一个药格 */
typedef struct {
    bool     used;
    char     name[PB_DRUG_NAME_LEN]; /* 药片名称（UTF-8） */
    uint16_t unit_mg;                /* 单片刻重（毫克） */
    uint16_t stock;                  /* 当前格内存放片数 */
    int32_t  empty_mg;               /* 空盒皮重基准（毫克） */
    uint16_t tolerance_pct;          /* 判定容差百分比，默认 15 */
    int32_t  allow_residual_mg;      /* 允许残留（掰半片场景），默认 0 */
    uint16_t take_window_s;          /* 取药窗口（秒），0 = 用全局默认 */
    uint8_t  dose_n;                 /* 有效 dose 个数，0 = 不带提醒（纯收纳格） */
    pb_dose_point_t dose[PB_MAX_DOSES_PER_DAY];

    /* 运行时字段（不要由 APP 下发） */
    int32_t  fired_day;  /* 记录已触发的"天序号"，跨天自动清零 */
    uint8_t  fired_mask; /* 当天已触发的 dose 位图 */
} pb_slot_t;

typedef struct {
    uint8_t  slot_count;          /* 实际使用格数 1..PB_MAX_SLOTS */
    uint32_t noise_mg;            /* 称重噪声门限（毫克），默认 150 */
    uint16_t settle_ms;           /* 关盖后稳定等待，默认 3000 */
    uint16_t verify_timeout_ms;   /* 等重量读数超时 */
    uint16_t take_window_s;       /* 默认取药窗口，默认 900 */
    uint16_t due_grace_min;       /* 到点补触发宽限（分钟），默认 10 */
    uint16_t remind_interval_s;   /* MISSED 重复提醒间隔，默认 300 */
    uint8_t  max_retry;           /* 未取完/未取 重试次数，默认 2 */
    uint8_t  battery_warn_pct;    /* 20 */
    uint8_t  battery_low_pct;     /* 10 */
    uint8_t  battery_critical_pct;/* 5  → 解锁进 STORAGE */
    uint32_t battery_cutoff_mv;   /* 3450 → 无论百分比，直接 STORAGE */
    int32_t  tz_offset_s;         /* 时区偏移，默认 +8h */
} pb_config_t;

typedef enum {
    PB_ST_INIT = 0,
    PB_ST_IDLE,
    PB_ST_DUE,
    PB_ST_WAIT_TAKE,
    PB_ST_TAKING,
    PB_ST_VERIFY,
    PB_ST_MISSED,
    PB_ST_PARTIAL,
    PB_ST_ESCALATE,
    PB_ST_STORAGE,
    PB_ST_FAULT,
} pb_state_t;

/* 取药判定结果 */
typedef enum {
    PB_DOSE_OK = 0,      /* 取够 */
    PB_DOSE_PARTIAL = 1, /* 取了一部分 / 未清零（残留超标） */
    PB_DOSE_NONE = 2,    /* 完全没取 */
    PB_DOSE_OVER = 3,    /* 多取 */
} pb_dose_result_t;

typedef struct {
    pb_dose_result_t result;
    int32_t expect_mg;   /* 应减少重量 */
    int32_t actual_mg;   /* 实际减少重量 */
    int32_t remain_mg;   /* 格内实际剩余（相对空盒） */
    int32_t residual_mg; /* 相对"应为剩余"的偏差，>0 表示残留超标 */
} pb_dose_eval_t;

/* 不透明句柄 */
typedef struct pb_core pb_core_t;

/* ---------------- 生命周期 ---------------- */

pb_config_t pb_default_config(void);
pb_core_t  *pb_core_create(const pb_hal_t *hal, const pb_config_t *cfg);
void        pb_core_destroy(pb_core_t *c);

/* 下发/更新药单（会话进行中调用会返回 PB_ERR_BUSY） */
pb_err_t    pb_core_set_slots(pb_core_t *c, const pb_slot_t *slots, uint8_t n);

/* ---------------- 查询 ---------------- */

pb_state_t        pb_core_state(const pb_core_t *c);
uint8_t           pb_core_active_slot(const pb_core_t *c);
uint8_t           pb_core_slot_count(const pb_core_t *c);
const pb_slot_t  *pb_core_slot(const pb_core_t *c, uint8_t idx);
const char       *pb_state_name(pb_state_t st);
const char       *pb_dose_result_name(pb_dose_result_t r);
int32_t           pb_core_seconds_to_next_dose(const pb_core_t *c, int64_t now_s,
                                               int32_t *slot_out);
bool              pb_core_is_storage_mode(const pb_core_t *c);

/* ---------------- 驱动 ---------------- */

/* 固件里由 1Hz 任务调用；now_ms 为系统毫秒（单调递增） */
void pb_core_tick(pb_core_t *c, uint32_t now_ms);

/* 命令（可由按键 / MQTT / BLE / 串口控制台调用） */
void pb_core_cmd_unlock_slot(pb_core_t *c, uint8_t slot, uint32_t now_ms);
void pb_core_cmd_unlock_all(pb_core_t *c, uint32_t now_ms);
void pb_core_cmd_ack(pb_core_t *c, uint32_t now_ms);
void pb_core_cmd_storage(pb_core_t *c, uint32_t now_ms);
void pb_core_cmd_silence(pb_core_t *c);
void pb_core_cmd_recheck_battery(pb_core_t *c, uint32_t now_ms);

/* 上层按键处理：返回 true 表示已消费 */
bool pb_core_on_button(pb_core_t *c, uint8_t key_id, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* PB_PILLBOX_H */
