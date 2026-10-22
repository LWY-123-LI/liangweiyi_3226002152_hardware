/*
 * stock.c —— 库存与药单校验
 */
#include "stock.h"

#include <string.h>

uint16_t pb_stock_consume(pb_slot_t *s, uint16_t units)
{
    if (s == NULL) {
        return 0;
    }
    if (units >= s->stock) {
        s->stock = 0;
        return 0;
    }
    s->stock = (uint16_t)(s->stock - units);
    return s->stock;
}

uint16_t pb_stock_add(pb_slot_t *s, uint16_t units)
{
    if (s == NULL) {
        return 0;
    }
    const uint32_t total = (uint32_t)s->stock + units;
    s->stock = (total > 0xFFFFu) ? 0xFFFFu : (uint16_t)total;
    return s->stock;
}

uint16_t pb_stock_days_left(const pb_slot_t *s)
{
    if (s == NULL || !s->used || s->dose_n == 0) {
        return 0;
    }
    uint32_t per_day = 0;
    for (uint8_t i = 0; i < s->dose_n && i < PB_MAX_DOSES_PER_DAY; i++) {
        per_day += s->dose[i].units;
    }
    if (per_day == 0) {
        return 0;
    }
    return (uint16_t)(s->stock / per_day);
}

bool pb_slot_validate(const pb_slot_t *s)
{
    if (s == NULL) {
        return false;
    }
    if (!s->used) {
        return true; /* 空位允许 */
    }
    if (s->unit_mg == 0 || s->unit_mg > 5000) {
        return false;
    }
    if (s->tolerance_pct > 80) {
        return false;
    }
    if (s->dose_n > PB_MAX_DOSES_PER_DAY) {
        return false;
    }
    for (uint8_t d = 0; d < s->dose_n; d++) {
        if (s->dose[d].minute >= 1440) {
            return false;
        }
        if (s->dose[d].units == 0 || s->dose[d].units > 20) {
            return false;
        }
    }
    return true;
}
