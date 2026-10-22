/*
 * mock_hal.h —— 主机测试用的模拟硬件
 */
#ifndef PB_MOCK_HAL_H
#define PB_MOCK_HAL_H

#include "hal_if.h"
#include "pillbox.h"

#define MOCK_MAX_EVENTS 192

typedef struct {
    /* ---- 可注入的硬件状态 ---- */
    int32_t  weight_mg[PB_MAX_SLOTS];
    bool     weight_fail[PB_MAX_SLOTS];
    bool     lid[PB_MAX_SLOTS];
    uint8_t  soc;
    uint32_t mv;
    bool     power_fail;
    int64_t  now_s;
    bool     time_valid;

    /* ---- 观测到的硬件动作 ---- */
    bool locked[PB_MAX_SLOTS];
    int  unlock_cnt[PB_MAX_SLOTS];
    int  lock_cnt[PB_MAX_SLOTS];

    int      speak_cnt;
    uint16_t last_phrase;
    char     last_text[PB_DRUG_NAME_LEN];

    int     ble_cnt;
    uint8_t last_alert;
    uint8_t last_ble_slot;

    int disp_due_cnt;
    int disp_idle_cnt;
    int disp_msg_cnt;
    int disp_storage_cnt;
    int speak_stop_cnt;

    char events[MOCK_MAX_EVENTS][320];
    int  event_cnt;
} mock_t;

void      mock_init(mock_t *m);
pb_hal_t  mock_hal(mock_t *m);

/* 设置"当天第几天 + 时分秒"（tz 由 cfg 决定，测试里一般用 0） */
void      mock_set_time(mock_t *m, int day, int hour, int minute, int second);

int         mock_event_count(const mock_t *m, const char *type);
const char *mock_last_event(const mock_t *m, const char *type);
bool        mock_event_contains(const mock_t *m, const char *type, const char *needle);

#endif /* PB_MOCK_HAL_H */
