/* test_rig.h —— 状态机测试夹具：mock 硬件 + 1Hz 时钟推进 */
#ifndef PB_TEST_RIG_H
#define PB_TEST_RIG_H

#include "mock_hal.h"
#include "pillbox.h"
#include "test_util.h"

#include <stdio.h>
#include <string.h>

#define PB_TEST_DAY 20000
#define PB_TEST_FULL_MG (1000 + 10 * 200) /* 空盒 1000 + 10 片 × 200mg */

typedef struct {
    mock_t     m;
    pb_core_t *core;
    uint32_t   t_ms;
} rig_t;

/* 一格：每日一次，08:00 / 20:00 等 */
static inline pb_slot_t rig_slot(uint16_t minute, uint8_t units)
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

/* 启动：时间 hour:minute，槽位与药单由调用方随后 pb_core_set_slots */
static inline bool rig_boot(rig_t *r, uint8_t n_slots, int hour, int minute)
{
    memset(r, 0, sizeof(*r));
    mock_init(&r->m);
    mock_set_time(&r->m, PB_TEST_DAY, hour, minute, 0);
    for (int i = 0; i < PB_MAX_SLOTS; i++) {
        r->m.weight_mg[i] = PB_TEST_FULL_MG;
    }

    pb_config_t cfg = pb_default_config();
    cfg.slot_count        = n_slots;
    cfg.tz_offset_s       = 0;
    cfg.take_window_s     = 60;
    cfg.settle_ms         = 200;
    cfg.verify_timeout_ms = 1000;
    cfg.remind_interval_s = 60;
    cfg.max_retry         = 1;
    cfg.due_grace_min     = 10;

    pb_hal_t hal = mock_hal(&r->m);
    r->core = pb_core_create(&hal, &cfg);
    r->t_ms = 1000;
    return r->core != NULL;
}

static inline void rig_drop(rig_t *r)
{
    pb_core_destroy(r->core);
    r->core = NULL;
}

static inline void rig_tick(rig_t *r)
{
    pb_core_tick(r->core, r->t_ms);
}

/* 推进 sec 秒，每 1000ms 调一次 tick（模拟 1Hz 主循环） */
static inline void rig_step(rig_t *r, uint32_t sec)
{
    for (uint32_t i = 0; i < sec; i++) {
        r->m.now_s += 1;
        r->t_ms += 1000;
        pb_core_tick(r->core, r->t_ms);
    }
}

static inline void rig_set_weight(rig_t *r, uint8_t slot, int32_t mg)
{
    r->m.weight_mg[slot] = mg;
}

static inline void rig_open(rig_t *r, uint8_t slot)
{
    r->m.lid[slot] = true;
}

static inline void rig_close(rig_t *r, uint8_t slot)
{
    r->m.lid[slot] = false;
}

static inline pb_state_t rig_state(rig_t *r)
{
    return pb_core_state(r->core);
}

static inline uint16_t rig_stock(rig_t *r, uint8_t slot)
{
    const pb_slot_t *s = pb_core_slot(r->core, slot);
    return (s != NULL) ? s->stock : 0;
}

#endif /* PB_TEST_RIG_H */
