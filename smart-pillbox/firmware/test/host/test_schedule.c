/* test_schedule.c —— 排程与库存纯函数 */
#include "test_util.h"

#include "schedule.h"
#include "stock.h"

#include <string.h>

#define DAY_S 86400

static pb_slot_t slot_at(uint16_t minute, uint8_t units)
{
    pb_slot_t s;
    memset(&s, 0, sizeof(s));
    s.used = true;
    snprintf(s.name, sizeof(s.name), "%s", "药A");
    s.unit_mg = 200;
    s.stock = 10;
    s.empty_mg = 1000;
    s.tolerance_pct = 15;
    s.dose_n = 1;
    s.dose[0].minute = minute;
    s.dose[0].units = units;
    return s;
}

void test_schedule_and_stock(void)
{
    const int32_t tz8 = 8 * 3600;
    const int64_t day0 = (int64_t)20000 * DAY_S;

    /* ---- 天序号 / 分钟 ---- */
    CHECK_EQI(pb_day_index(day0, 0), 20000);
    CHECK_EQI(pb_day_index(day0 + 23 * 3600, 0), 20000);
    CHECK_EQI(pb_day_index(day0 - 1, 0), 19999);
    /* UTC 00:30 = 北京时间 08:30 */
    CHECK_EQI(pb_minute_of_day(day0 + 30 * 60, tz8), 8 * 60 + 30);
    CHECK_EQI(pb_minute_of_day(day0 + 30 * 60, 0), 30);
    /* 跨天：北京时间次日 00:10 → 分钟回到 10，天序号 +1 */
    CHECK_EQI(pb_minute_of_day(day0 + 16 * 3600 + 10 * 60, tz8), 10);
    CHECK_EQI(pb_day_index(day0 + 16 * 3600 + 10 * 60, tz8), 20001);

    /* ---- 到点判定 ---- */
    pb_slot_t s = slot_at(480, 1); /* 08:00 */
    s.fired_day = 20000;

    CHECK_EQI(pb_dose_due(&s, 0, 20000, 479, 10), 0); /* 差 1 分钟：不触发 */
    CHECK_EQI(pb_dose_due(&s, 0, 20000, 480, 10), 1); /* 正点 */
    CHECK_EQI(pb_dose_due(&s, 0, 20000, 485, 10), 1); /* 宽限内（补提醒） */
    CHECK_EQI(pb_dose_due(&s, 0, 20000, 490, 10), 1); /* 宽限边界 */
    CHECK_EQI(pb_dose_due(&s, 0, 20000, 491, 10), 0); /* 超出宽限 */
    CHECK_EQI(pb_dose_due(&s, 0, 20000, 600, 10), 0);

    /* 已触发过就不再触发 */
    s.fired_mask = 0x01;
    CHECK_EQI(pb_dose_due(&s, 0, 20000, 480, 10), 0);
    /* 换天：fired_mask 归零后允许再次触发 */
    s.fired_day = 20001;
    s.fired_mask = 0;
    CHECK_EQI(pb_dose_due(&s, 0, 20001, 480, 10), 1);

    /* dose_idx 越界 / 未启用的格子 */
    CHECK_EQI(pb_dose_due(&s, 3, 20001, 480, 10), 0);
    pb_slot_t unused = slot_at(480, 1);
    unused.used = false;
    CHECK_EQI(pb_dose_due(&unused, 0, 20000, 480, 10), 0);

    /* ---- 下一次服药倒计时 ---- */
    pb_slot_t slots[2];
    memset(slots, 0, sizeof(slots));
    slots[0] = slot_at(480, 1); /* 08:00 */
    slots[1] = slot_at(1200, 1); /* 20:00，第二格 */

    int32_t which = -1;
    /* 07:00 → 还有 1 小时 */
    int32_t sec = pb_seconds_to_next(slots, 2, day0 + 7 * 3600, 0, &which);
    CHECK_EQI(sec, 3600);
    CHECK_EQI(which, 0);

    /* 09:00 → 已过 08:00，下一次 20:00 → 11 小时 */
    sec = pb_seconds_to_next(slots, 2, day0 + 9 * 3600, 0, &which);
    CHECK_EQI(sec, 11 * 3600);
    CHECK_EQI(which, 1);

    /* 21:00 → 明天 08:00 → 11 小时，回到第 0 格 */
    sec = pb_seconds_to_next(slots, 2, day0 + 21 * 3600, 0, &which);
    CHECK_EQI(sec, 11 * 3600);
    CHECK_EQI(which, 0);

    /* 无排程 */
    slots[0].dose_n = 0;
    slots[1].dose_n = 0;
    CHECK_EQI(pb_seconds_to_next(slots, 2, day0, 0, &which), -1);
    CHECK_EQI(which, -1);

    /* ---- 库存 ---- */
    pb_slot_t st = slot_at(480, 2);
    st.stock = 10;
    CHECK_EQI(pb_stock_consume(&st, 3), 7);
    CHECK_EQI(pb_stock_add(&st, 5), 12);
    CHECK_EQI(pb_stock_days_left(&st), 6); /* 12 片 / 每天 2 片 */
    CHECK_EQI(pb_stock_consume(&st, 12), 0); /* 不足时归零 */
    CHECK_EQI(pb_stock_consume(&st, 1), 0);

    /* ---- 药单校验 ---- */
    pb_slot_t v = slot_at(480, 1);
    CHECK(pb_slot_validate(&v));
    v.unit_mg = 0;
    CHECK(!pb_slot_validate(&v));
    v = slot_at(480, 1);
    v.dose[0].units = 0;
    CHECK(!pb_slot_validate(&v));
    v = slot_at(1440, 1);
    CHECK(!pb_slot_validate(&v));
    v = slot_at(480, 1);
    v.used = false;
    CHECK(pb_slot_validate(&v)); /* 空位允许，视为纯收纳格 */
}
