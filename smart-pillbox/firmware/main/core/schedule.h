/*
 * schedule.h —— 服药排程计算（纯函数）
 */
#ifndef PB_SCHEDULE_H
#define PB_SCHEDULE_H

#include "pillbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 本地"天序号"（用于跨天标记） */
int32_t  pb_day_index(int64_t unix_sec, int32_t tz_offset_s);

/* 本地"当天第几分钟" 0..1439 */
uint16_t pb_minute_of_day(int64_t unix_sec, int32_t tz_offset_s);

/* 距离下一次 dose 还有多少秒（含跨天），slot_out 返回格子下标，无排程返回 -1 */
int32_t  pb_seconds_to_next(const pb_slot_t *slots, uint8_t n,
                           int64_t now_s, int32_t tz_offset_s,
                           int32_t *slot_out);

/*
 * 是否需要触发某个 dose：
 *   - 到达时刻当分钟内；
 *   - 或已过点但仍在 due_grace_min 宽限内（断电/重启后补提醒）；
 *   - 当天同一 dose 只触发一次（fired_mask 由调用方维护）。
 * 返回 1 需要触发，0 不需要。
 */
int pb_dose_due(const pb_slot_t *s, uint8_t dose_idx, int32_t day,
                uint16_t now_minute, uint16_t grace_min);

#ifdef __cplusplus
}
#endif

#endif /* PB_SCHEDULE_H */
