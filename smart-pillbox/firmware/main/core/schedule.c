#include "schedule.h"

#include <stddef.h>

#define PB_MINUTES_PER_DAY 1440
#define PB_SECONDS_PER_DAY 86400

int32_t pb_day_index(int64_t unix_sec, int32_t tz_offset_s)
{
    int64_t local = unix_sec + (int64_t)tz_offset_s;
    int64_t day = local / PB_SECONDS_PER_DAY;
    if (local < 0 && (local % PB_SECONDS_PER_DAY) != 0) {
        day -= 1;
    }
    return (int32_t)day;
}

uint16_t pb_minute_of_day(int64_t unix_sec, int32_t tz_offset_s)
{
    int64_t local = unix_sec + (int64_t)tz_offset_s;
    int64_t sod = local % PB_SECONDS_PER_DAY;
    if (sod < 0) {
        sod += PB_SECONDS_PER_DAY;
    }
    return (uint16_t)(sod / 60);
}

int pb_dose_due(const pb_slot_t *s, uint8_t dose_idx, int32_t day,
                uint16_t now_minute, uint16_t grace_min)
{
    if (s == NULL || !s->used || dose_idx >= s->dose_n) {
        return 0;
    }
    if (s->fired_day == day && (s->fired_mask & (1u << dose_idx)) != 0) {
        return 0; /* 当天已触发 */
    }

    const uint16_t target = s->dose[dose_idx].minute % PB_MINUTES_PER_DAY;
    if (now_minute < target) {
        return 0;
    }
    if ((uint32_t)(now_minute - target) <= (uint32_t)grace_min) {
        return 1;
    }
    return 0;
}

int32_t pb_seconds_to_next(const pb_slot_t *slots, uint8_t n,
                           int64_t now_s, int32_t tz_offset_s,
                           int32_t *slot_out)
{
    if (slots == NULL || n == 0) {
        if (slot_out != NULL) {
            *slot_out = -1;
        }
        return -1;
    }

    int64_t local = now_s + (int64_t)tz_offset_s;
    int64_t sod = local % PB_SECONDS_PER_DAY;
    if (sod < 0) {
        sod += PB_SECONDS_PER_DAY;
    }
    const int32_t now_min = (int32_t)(sod / 60);

    int32_t best = -1;
    int32_t best_slot = -1;

    for (uint8_t i = 0; i < n; i++) {
        const pb_slot_t *s = &slots[i];
        if (!s->used || s->dose_n == 0) {
            continue;
        }
        for (uint8_t d = 0; d < s->dose_n && d < PB_MAX_DOSES_PER_DAY; d++) {
            int32_t diff_min = (int32_t)(s->dose[d].minute % PB_MINUTES_PER_DAY) - now_min;
            if (diff_min < 0) {
                diff_min += PB_MINUTES_PER_DAY;
            }
            const int32_t diff_s = diff_min * 60;
            if (best < 0 || diff_s < best) {
                best = diff_s;
                best_slot = (int32_t)i;
            }
        }
    }

    if (slot_out != NULL) {
        *slot_out = best_slot;
    }
    return best;
}
