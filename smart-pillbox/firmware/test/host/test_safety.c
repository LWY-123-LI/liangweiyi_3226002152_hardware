/* test_safety.c —— 电源降级、远程控制、边界保护 */
#include "test_rig.h"

/* 1) 电量分档提醒（不进入收纳盒模式） */
static void s_low_battery_warn(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 7, 0));
    pb_slot_t sl = rig_slot(600, 1);
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);

    r.m.soc = 15; /* 20% 以下 → level 1 */
    rig_tick(&r);
    CHECK_EQI(mock_event_count(&r.m, "low_battery"), 1);
    CHECK(mock_event_contains(&r.m, "low_battery", "\"level\":1"));
    CHECK_EQI(r.m.last_phrase, PB_PHRASE_LOW_BATTERY);
    CHECK(!pb_core_is_storage_mode(r.core));
    CHECK(rig_state(&r) == PB_ST_IDLE);

    /* 同档位不重复刷屏 */
    rig_step(&r, 30);
    CHECK_EQI(mock_event_count(&r.m, "low_battery"), 1);

    /* 继续掉到 8% → level 2 再报一次 */
    r.m.soc = 8;
    rig_step(&r, 10);
    CHECK_EQI(mock_event_count(&r.m, "low_battery"), 2);
    CHECK(mock_event_contains(&r.m, "low_battery", "\"level\":2"));
    CHECK(!pb_core_is_storage_mode(r.core));
    rig_drop(&r);
}

/* 2) 电量临界 → 全部解锁进收纳盒模式 */
static void s_storage_mode_by_soc(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 2, 7, 0));
    pb_slot_t sl[2];
    sl[0] = rig_slot(480, 1);
    sl[1] = rig_slot(600, 1);
    CHECK_EQI(pb_core_set_slots(r.core, sl, 2), PB_OK);
    rig_tick(&r);
    CHECK_EQI(r.m.locked[0], true);
    CHECK_EQI(r.m.locked[1], true);

    r.m.soc = 4; /* ≤ 5% → 立即解锁 */
    rig_step(&r, 6);
    CHECK(rig_state(&r) == PB_ST_STORAGE);
    CHECK(pb_core_is_storage_mode(r.core));
    CHECK(!r.m.locked[0]);
    CHECK(!r.m.locked[1]);
    CHECK_EQI(mock_event_count(&r.m, "storage_mode"), 1);
    CHECK_EQI(r.m.last_alert, PB_ALERT_STORAGE);
    CHECK_EQI(r.m.disp_storage_cnt, 1);

    /* 充电后也不自动回退（必须重新下发药单） */
    r.m.soc = 90;
    r.m.mv = 4050;
    rig_step(&r, 30);
    CHECK(rig_state(&r) == PB_ST_STORAGE);
    CHECK(pb_core_is_storage_mode(r.core));
    CHECK_EQI(mock_event_count(&r.m, "storage_mode"), 1);

    /* 重新下发药单 → 恢复工作模式并重新上锁 */
    CHECK_EQI(pb_core_set_slots(r.core, sl, 2), PB_OK);
    CHECK(rig_state(&r) == PB_ST_IDLE);
    CHECK(!pb_core_is_storage_mode(r.core));
    rig_step(&r, 2);
    CHECK(r.m.locked[0]);
    CHECK(r.m.locked[1]);
    rig_drop(&r);
}

/* 3) 电压兜底：SOC 还没掉到底但电压已到截止值 */
static void s_storage_mode_by_voltage(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 7, 0));
    pb_slot_t sl = rig_slot(600, 1);
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);
    rig_tick(&r);

    r.m.soc = 40;
    r.m.mv = 3400;
    rig_step(&r, 6);
    CHECK(rig_state(&r) == PB_ST_STORAGE);
    CHECK(mock_event_contains(&r.m, "storage_mode", "\"mv\":3400"));
    rig_drop(&r);
}

/* 4) 手动/远程开格：120s 后自动回锁 */
static void s_remote_unlock_autolock(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 2, 7, 0));
    pb_slot_t sl[2];
    sl[0] = rig_slot(600, 1);
    sl[1] = rig_slot(660, 1);
    CHECK_EQI(pb_core_set_slots(r.core, sl, 2), PB_OK);
    rig_tick(&r);

    pb_core_cmd_unlock_slot(r.core, 1, r.t_ms);
    CHECK(!r.m.locked[1]);
    CHECK(r.m.locked[0]);
    CHECK_EQI(mock_event_count(&r.m, "remote_unlock"), 1);
    CHECK(mock_event_contains(&r.m, "remote_unlock", "\"slot\":2,\"ttlS\":120"));

    rig_step(&r, 119);
    CHECK(!r.m.locked[1]); /* 还没到期 */
    rig_step(&r, 3);
    CHECK(r.m.locked[1]);
    CHECK_EQI(mock_event_count(&r.m, "auto_lock"), 1);

    /* 全部开锁 */
    pb_core_cmd_unlock_all(r.core, r.t_ms);
    CHECK(!r.m.locked[0] && !r.m.locked[1]);
    CHECK_EQI(mock_event_count(&r.m, "remote_unlock"), 2);
    rig_drop(&r);
}

/* 5) 库存不足：不进入取药流程，只上报 + 提示补药 */
static void s_stock_empty(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 7, 59));
    pb_slot_t sl = rig_slot(480, 2); /* 需取 2 片，但只剩 1 片 */
    sl.stock = 1;
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);
    rig_tick(&r);
    rig_step(&r, 60);

    CHECK_EQI(mock_event_count(&r.m, "stock_empty"), 1);
    CHECK(mock_event_contains(&r.m, "stock_empty", "\"stock\":1,\"units\":2"));
    CHECK(rig_state(&r) == PB_ST_IDLE);
    CHECK_EQI(r.m.unlock_cnt[0], 0);
    CHECK_EQI(r.m.disp_msg_cnt, 1);

    /* 库存被补上后，次日同一时刻仍可正常提醒 */
    rig_step(&r, 60);
    CHECK(rig_state(&r) == PB_ST_IDLE);
    rig_drop(&r);
}

/* 6) 会话进行中拒绝改药单（避免状态错乱） */
static void s_set_slots_busy(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 7, 59));
    pb_slot_t sl = rig_slot(480, 1);
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);
    rig_tick(&r);
    rig_step(&r, 60);
    CHECK(rig_state(&r) == PB_ST_WAIT_TAKE);

    pb_slot_t other = rig_slot(600, 1);
    CHECK_EQI(pb_core_set_slots(r.core, &other, 1), PB_ERR_BUSY);
    CHECK_EQI(pb_core_slot(r.core, 0)->dose[0].minute, 480); /* 原药单未被破坏 */

    /* 参数非法 */
    CHECK_EQI(pb_core_set_slots(r.core, NULL, 1), PB_ERR_INVALID);
    CHECK_EQI(pb_core_set_slots(r.core, &other, 0), PB_ERR_INVALID);
    other.unit_mg = 0;
    CHECK_EQI(pb_core_set_slots(r.core, &other, 1), PB_ERR_INVALID);
    rig_drop(&r);
}

/* 7) 称重读取失败 → FAULT（不误判成"没取药"） */
static void s_weight_fault(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 7, 59));
    pb_slot_t sl = rig_slot(480, 1);
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);
    rig_tick(&r);
    rig_step(&r, 60);

    rig_open(&r, 0);
    rig_step(&r, 1);
    r.m.weight_fail[0] = true;
    rig_close(&r, 0);
    rig_step(&r, 4); /* settle 200ms + verify_timeout 1000ms 之后仍读不到 */
    CHECK(rig_state(&r) == PB_ST_FAULT);
    CHECK_EQI(mock_event_count(&r.m, "fault"), 1);
    CHECK(mock_event_contains(&r.m, "fault", "\"reason\":\"weight_read\""));
    CHECK_EQI(mock_event_count(&r.m, "dose_none"), 0);
    rig_drop(&r);
}

/* 8) 按键：静音 / 确认（升级告警下解除） */
static void s_buttons(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 7, 59));
    pb_slot_t sl = rig_slot(480, 1);
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);
    rig_tick(&r);
    rig_step(&r, 60);

    CHECK(pb_core_on_button(r.core, 1, r.t_ms)); /* 静音 */
    CHECK_EQI(mock_event_count(&r.m, "silence"), 1);
    CHECK_EQI(r.m.speak_stop_cnt, 1);
    CHECK(!pb_core_on_button(r.core, 9, r.t_ms)); /* 未知按键不消费 */

    /* 走到升级告警，再按确认键解除 */
    rig_step(&r, 61);
    rig_step(&r, 61);
    rig_step(&r, 61);
    CHECK(rig_state(&r) == PB_ST_ESCALATE);
    CHECK(pb_core_on_button(r.core, 0, r.t_ms));
    CHECK(rig_state(&r) == PB_ST_IDLE);
    CHECK_EQI(mock_event_count(&r.m, "ack"), 1);
    rig_drop(&r);
}

/* 9) 电量复检命令 + 手工进入收纳盒模式 */
static void s_manual_commands(void)
{
    rig_t r;
    CHECK(rig_boot(&r, 1, 7, 0));
    pb_slot_t sl = rig_slot(600, 1);
    CHECK_EQI(pb_core_set_slots(r.core, &sl, 1), PB_OK);
    rig_tick(&r);

    /* 电池驱动暂时读不到 → 不应误进收纳盒 */
    r.m.power_fail = true;
    pb_core_cmd_recheck_battery(r.core, r.t_ms);
    CHECK(!pb_core_is_storage_mode(r.core));

    r.m.power_fail = false;
    r.m.soc = 30;
    pb_core_cmd_recheck_battery(r.core, r.t_ms);
    CHECK(!pb_core_is_storage_mode(r.core));

    pb_core_cmd_storage(r.core, r.t_ms);
    CHECK(pb_core_is_storage_mode(r.core));
    CHECK(rig_state(&r) == PB_ST_STORAGE);
    CHECK(!r.m.locked[0]);
    rig_drop(&r);
}

void test_safety(void)
{
    s_low_battery_warn();
    s_storage_mode_by_soc();
    s_storage_mode_by_voltage();
    s_remote_unlock_autolock();
    s_stock_empty();
    s_set_slots_busy();
    s_weight_fault();
    s_buttons();
    s_manual_commands();
}

