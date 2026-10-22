/* test_session.c —— 取药会话状态机主流程 */
#include "test_rig.h"

/* 1) 到点 → 开盖 → 取够 → 回空闲，库存 -1 */
static void s_dose_ok(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 7, 59));
    pb_slot_t sl = rig_slot(480, 1);
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);

    rig_tick(&r); /* INIT → IDLE，上电全部闭锁 */
    CHECK(rig_state(&r) == PB_ST_IDLE);
    CHECK_EQI(r.m.lock_cnt[0], 1);

    rig_step(&r, 60); /* 走到 08:00 */
    CHECK(rig_state(&r) == PB_ST_WAIT_TAKE);
    CHECK_EQI(pb_core_active_slot(r.core), 0);
    CHECK_EQI(r.m.unlock_cnt[0], 1);
    CHECK_EQI(r.m.disp_due_cnt, 1);
    CHECK_EQI(r.m.last_phrase, PB_PHRASE_DUE);
    CHECK_EQI(mock_event_count(&r.m, "dose_due"), 1);
    CHECK(mock_event_contains(&r.m, "dose_due", "\"expectMg\":200"));

    rig_open(&r, 0);
    rig_step(&r, 1);
    CHECK(rig_state(&r) == PB_ST_TAKING);
    CHECK_EQI(mock_event_count(&r.m, "lid_open"), 1);

    rig_set_weight(&r, 0, 2800); /* 取走 1 片 */
    rig_close(&r, 0);
    rig_step(&r, 1);
    CHECK(rig_state(&r) == PB_ST_VERIFY);

    rig_step(&r, 1); /* 关盖稳定 200ms 后判定 */
    CHECK(rig_state(&r) == PB_ST_IDLE);
    CHECK_EQI(rig_stock(&r, 0), 9);
    CHECK(r.m.locked[0]); /* 判定完自动回锁 */
    CHECK_EQI(mock_event_count(&r.m, "dose_ok"), 1);
    CHECK(mock_event_contains(&r.m, "dose_ok", "\"actualMg\":200"));
    CHECK_EQI(r.m.last_phrase, PB_PHRASE_DOSE_OK);

    /* 同一天不重复触发 */
    rig_step(&r, 120);
    CHECK_EQI(mock_event_count(&r.m, "dose_due"), 1);
    CHECK(rig_state(&r) == PB_ST_IDLE);
    rig_drop(&r);
}

/* 2) 开盖但没取 → NONE → 重试 → 取够 */
static void s_none_retry_ok(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 7, 59));
    pb_slot_t sl = rig_slot(480, 1);
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);
    rig_tick(&r);
    rig_step(&r, 60);
    CHECK(rig_state(&r) == PB_ST_WAIT_TAKE);

    rig_open(&r, 0);
    rig_step(&r, 1);
    rig_close(&r, 0); /* 重量没变 */
    rig_step(&r, 1);
    CHECK(rig_state(&r) == PB_ST_VERIFY);
    rig_step(&r, 1);

    CHECK(rig_state(&r) == PB_ST_WAIT_TAKE); /* 重试，重新开格 */
    CHECK_EQI(mock_event_count(&r.m, "dose_none"), 1);
    CHECK_EQI(r.m.unlock_cnt[0], 2);
    CHECK_EQI(r.m.last_phrase, PB_PHRASE_MISSED);

    rig_open(&r, 0);
    rig_step(&r, 1);
    rig_set_weight(&r, 0, 2800);
    rig_close(&r, 0);
    rig_step(&r, 2);
    CHECK(rig_state(&r) == PB_ST_IDLE);
    CHECK_EQI(rig_stock(&r, 0), 9);
    CHECK_EQI(mock_event_count(&r.m, "dose_ok"), 1);
    rig_drop(&r);
}

/* 3) 该取 2 片只取 1 片 → PARTIAL → 重试时补齐 → 累计达标 */
static void s_partial_retry_ok(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 7, 59));
    pb_slot_t sl = rig_slot(480, 2); /* 一次 2 片 */
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);
    rig_tick(&r);
    rig_step(&r, 60);

    rig_open(&r, 0);
    rig_step(&r, 1);
    rig_set_weight(&r, 0, 2800); /* 只取走 1 片（200mg / 应取 400mg） */
    rig_close(&r, 0);
    rig_step(&r, 2);

    CHECK(rig_state(&r) == PB_ST_WAIT_TAKE);
    CHECK_EQI(mock_event_count(&r.m, "dose_partial"), 1);
    CHECK(mock_event_contains(&r.m, "dose_partial", "\"actualMg\":200"));
    CHECK_EQI(r.m.last_phrase, PB_PHRASE_NOT_FINISHED);
    CHECK_EQI(r.m.unlock_cnt[0], 2);
    CHECK_EQI(rig_stock(&r, 0), 10); /* 未达标不扣库存 */

    /* 重试基准不刷新：再取 1 片 → 累计 400mg → 达标 */
    rig_open(&r, 0);
    rig_step(&r, 1);
    rig_set_weight(&r, 0, 2600);
    rig_close(&r, 0);
    rig_step(&r, 2);
    CHECK(rig_state(&r) == PB_ST_IDLE);
    CHECK_EQI(rig_stock(&r, 0), 8);
    CHECK_EQI(mock_event_count(&r.m, "dose_ok"), 1);
    CHECK(mock_event_contains(&r.m, "dose_ok", "\"actualMg\":400"));
    rig_drop(&r);
}

/* 4) 一直不开盖 → 超时 → 重复提醒 → 升级告警 → 自动回空闲 */
static void s_missed_escalate(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 7, 59));
    pb_slot_t sl = rig_slot(480, 1);
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);
    rig_tick(&r);
    rig_step(&r, 60);
    CHECK(rig_state(&r) == PB_ST_WAIT_TAKE);

    rig_step(&r, 61); /* 取药窗口 60s 到期 */
    CHECK(rig_state(&r) == PB_ST_MISSED);
    CHECK_EQI(mock_event_count(&r.m, "dose_missed"), 1);
    CHECK_EQI(r.m.last_phrase, PB_PHRASE_MISSED);

    rig_step(&r, 61); /* 提醒间隔 60s → 再次提醒并重新开格 */
    CHECK(rig_state(&r) == PB_ST_WAIT_TAKE);
    CHECK_EQI(mock_event_count(&r.m, "dose_remind"), 1);
    CHECK_EQI(r.m.unlock_cnt[0], 2);

    rig_step(&r, 61); /* 第二次超时 → retry 2 > max_retry 1 → 升级 */
    CHECK(rig_state(&r) == PB_ST_ESCALATE);
    CHECK_EQI(mock_event_count(&r.m, "dose_escalate"), 1);
    CHECK(mock_event_contains(&r.m, "dose_escalate", "\"reason\":\"missed\""));
    CHECK(r.m.locked[0]); /* 告警时把格子锁回去 */
    CHECK_EQI(r.m.last_alert, PB_ALERT_ESCALATE);

    rig_step(&r, 61); /* 停留 60s 后回到空闲 */
    CHECK(rig_state(&r) == PB_ST_IDLE);
    CHECK_EQI(pb_core_active_slot(r.core), 0xFF);

    /* 结论：该次 dose 当天不再补触发（fired_mask 已置位） */
    rig_step(&r, 120);
    CHECK_EQI(mock_event_count(&r.m, "dose_due"), 1);
    rig_drop(&r);
}

/* 5) 开错格子 → 语音提示"请到指定药格取药" */
static void s_wrong_slot(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 2, 7, 59));
    pb_slot_t sl[2];
    sl[0] = rig_slot(480, 1);  /* 08:00 第 1 格 */
    sl[1] = rig_slot(600, 1);  /* 10:00 第 2 格 */
    CHECK_EQI(pb_core_set_slots(r.core, sl, 2), PB_OK);
    rig_tick(&r);
    rig_step(&r, 60);
    CHECK(rig_state(&r) == PB_ST_WAIT_TAKE);
    CHECK_EQI(pb_core_active_slot(r.core), 0);

    rig_open(&r, 1); /* 开的是第 2 格 */
    rig_step(&r, 2);
    CHECK_EQI(mock_event_count(&r.m, "wrong_slot"), 1);
    CHECK(mock_event_contains(&r.m, "wrong_slot", "\"expectSlot\":1,\"openedSlot\":2"));
    CHECK_EQI(r.m.last_phrase, PB_PHRASE_WRONG_SLOT);
    CHECK(rig_state(&r) == PB_ST_WAIT_TAKE); /* 不打断正常流程 */

    rig_close(&r, 1);
    rig_open(&r, 0);
    rig_step(&r, 1);
    CHECK(rig_state(&r) == PB_ST_TAKING);
    rig_drop(&r);
}

/* 6) RTC 未校时：不触发任何提醒（宁可不提醒，也不能乱提醒） */
static void s_no_time_no_due(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 7, 59));
    pb_slot_t sl = rig_slot(480, 1);
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);
    r.m.time_valid = false;
    rig_tick(&r);
    CHECK(rig_state(&r) == PB_ST_IDLE);
    rig_step(&r, 120);
    CHECK(rig_state(&r) == PB_ST_IDLE);
    CHECK_EQI(mock_event_count(&r.m, "dose_due"), 0);
    CHECK_EQI(r.m.unlock_cnt[0], 0);
    rig_drop(&r);
}

/* 7) 断电重启后仍在宽限内 → 补提醒 */
static void s_catch_up_within_grace(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 8, 5)); /* 已过点 5 分钟，宽限 10 分钟 */
    pb_slot_t sl = rig_slot(480, 1);
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);
    rig_tick(&r); /* INIT */
    rig_step(&r, 2);
    CHECK(rig_state(&r) == PB_ST_WAIT_TAKE);
    CHECK_EQI(mock_event_count(&r.m, "dose_due"), 1);

    rig_open(&r, 0);
    rig_step(&r, 1);
    rig_set_weight(&r, 0, 2800);
    rig_close(&r, 0);
    rig_step(&r, 2);
    CHECK_EQI(rig_stock(&r, 0), 9);
    rig_drop(&r);
}

/* 8) 超出宽限 → 不补提醒 */
static void s_no_catch_up_after_grace(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 8, 30)); /* 过点 30 分钟 */
    pb_slot_t sl = rig_slot(480, 1);
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);
    rig_tick(&r);
    rig_step(&r, 30);
    CHECK_EQI(mock_event_count(&r.m, "dose_due"), 0);
    CHECK(rig_state(&r) == PB_ST_IDLE);
    rig_drop(&r);
}

void test_session(void)
{
    s_dose_ok();
    s_none_retry_ok();
    s_partial_retry_ok();
    s_missed_escalate();
    s_wrong_slot();
    s_no_time_no_due();
    s_catch_up_within_grace();
    s_no_catch_up_after_grace();
}

