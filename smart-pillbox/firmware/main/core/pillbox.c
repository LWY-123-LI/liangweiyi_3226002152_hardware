/*
 * pillbox.c —— 取药会话状态机
 *
 * 这里不出现任何 ESP-IDF API，全部通过 pb_hal_t 调用硬件，
 * 因此同一份代码既跑在 ESP32-S3 上，也跑在 PC 单元测试里。
 */
#include "pillbox.h"

#include "dose_verify.h"
#include "schedule.h"
#include "stock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PB_EVENT_BUF            320u
#define PB_POWER_PERIOD_MS      5000u   /* 电量采样周期 */
#define PB_IDLE_REFRESH_MS      30000u  /* 空闲界面刷新周期 */
#define PB_ESCALATE_HOLD_MS     60000u  /* 升级告警停留时间 */
#define PB_REMOTE_UNLOCK_MS     120000u /* 远程开格后自动回锁 */

struct pb_core {
    pb_hal_t   hal;
    pb_config_t cfg;

    pb_slot_t  slots[PB_MAX_SLOTS];
    uint8_t    slot_count;

    pb_state_t state;
    uint8_t    active_slot;
    uint8_t    active_units;

    uint32_t   pending_mask;                      /* 待处理的到点药格位图 */
    uint8_t    pending_units[PB_MAX_SLOTS];        /* 每个待处理药格应取片数 */
    bool       locked[PB_MAX_SLOTS];               /* 软件侧的锁状态缓存 */
    bool       manual_active[PB_MAX_SLOTS];        /* 远程手动开格中 */
    uint32_t   manual_until_ms[PB_MAX_SLOTS];

    int32_t    w_before_mg;                       /* 本次会话开锁前重量：重试时累计比较的基准 */
    uint32_t   now_ms;

    uint32_t   t_enter_ms;
    uint32_t   t_lid_close_ms;
    uint32_t   t_power_ms;
    uint32_t   t_display_ms;

    uint8_t    retry;
    bool       lid_was_open;
    bool       wrong_slot_reported;

    bool       storage_latched;
    uint8_t    battery_level;                     /* 0 正常 / 1 提醒 / 2 低电 */
};

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static const char *k_state_names[] = {
    "init", "idle", "due", "wait_take", "taking", "verify",
    "missed", "partial", "escalate", "storage", "fault",
};

const char *pb_state_name(pb_state_t st)
{
    if ((unsigned)st >= sizeof(k_state_names) / sizeof(k_state_names[0])) {
        return "unknown";
    }
    return k_state_names[st];
}

const char *pb_dose_result_name(pb_dose_result_t r)
{
    switch (r) {
    case PB_DOSE_OK:      return "ok";
    case PB_DOSE_PARTIAL: return "partial";
    case PB_DOSE_NONE:    return "none";
    case PB_DOSE_OVER:    return "over";
    default:              return "unknown";
    }
}

/* JSON 字符串转义：保留 UTF-8 多字节，过滤控制字符 */
static void json_escape(char *dst, size_t n, const char *src)
{
    size_t j = 0;
    if (dst == NULL || n == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    for (size_t i = 0; src[i] != '\0' && (j + 2) < n; i++) {
        const unsigned char ch = (unsigned char)src[i];
        if (ch == '"' || ch == '\\') {
            dst[j++] = '\\';
            dst[j++] = (char)ch;
        } else if (ch < 0x20) {
            dst[j++] = ' ';
        } else {
            dst[j++] = (char)ch;
        }
    }
    dst[j] = '\0';
}

static bool time_ok(pb_core_t *c, int64_t *out)
{
    int64_t t = 0;
    if (c->hal.time == NULL || c->hal.time->unix_time == NULL) {
        return false;
    }
    if (c->hal.time->unix_time(c->hal.time->ctx, &t) != PB_OK) {
        return false;
    }
    if (c->hal.time->time_valid != NULL && !c->hal.time->time_valid(c->hal.time->ctx)) {
        return false;
    }
    if (out != NULL) {
        *out = t;
    }
    return true;
}

/* 组装并上报事件：{"ts":..,"type":"..",<extra>} */
static void emit(pb_core_t *c, const char *type, const char *extra)
{
    char buf[PB_EVENT_BUF];
    int64_t now = 0;
    const bool ok = time_ok(c, &now);
    int n = snprintf(buf, sizeof(buf), "{\"ts\":%lld,\"type\":\"%s\"",
                     (long long)(ok ? now : 0), type);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return;
    }
    if (extra != NULL && extra[0] != '\0') {
        const int n2 = snprintf(buf + n, sizeof(buf) - (size_t)n, ",%s", extra);
        if (n2 < 0) {
            return;
        }
        n += n2;
        if ((size_t)n >= sizeof(buf)) {
            return;
        }
    }
    snprintf(buf + n, sizeof(buf) - (size_t)n, "}");
    if (c->hal.notify != NULL && c->hal.notify->publish_event != NULL) {
        c->hal.notify->publish_event(c->hal.notify->ctx, buf);
    }
}

static void speak(pb_core_t *c, pb_phrase_t phrase, const char *text)
{
    if (c->hal.audio != NULL && c->hal.audio->speak != NULL) {
        c->hal.audio->speak(c->hal.audio->ctx, (uint16_t)phrase, text);
    }
}

static void ble_alert(pb_core_t *c, pb_alert_t alert, uint8_t slot)
{
    if (c->hal.notify != NULL && c->hal.notify->notify_bracelet != NULL) {
        c->hal.notify->notify_bracelet(c->hal.notify->ctx, (uint8_t)alert, slot);
    }
}

static bool lid_open(pb_core_t *c, uint8_t slot, bool *open)
{
    bool v = false;
    if (c->hal.lid == NULL || c->hal.lid->is_open == NULL) {
        if (open != NULL) {
            *open = false;
        }
        return false;
    }
    if (c->hal.lid->is_open(c->hal.lid->ctx, slot, &v) != PB_OK) {
        return false;
    }
    if (open != NULL) {
        *open = v;
    }
    return true;
}

static bool read_weight(pb_core_t *c, uint8_t slot, int32_t *mg)
{
    if (c->hal.weight == NULL || c->hal.weight->read_mg == NULL) {
        return false;
    }
    return c->hal.weight->read_mg(c->hal.weight->ctx, slot, mg) == PB_OK;
}

static void do_unlock(pb_core_t *c, uint8_t slot)
{
    if (c->hal.lock != NULL && c->hal.lock->unlock != NULL) {
        c->hal.lock->unlock(c->hal.lock->ctx, slot);
    }
    c->locked[slot] = false;
}

static void do_lock(pb_core_t *c, uint8_t slot)
{
    if (c->hal.lock != NULL && c->hal.lock->lock != NULL) {
        c->hal.lock->lock(c->hal.lock->ctx, slot);
    }
    c->locked[slot] = true;
}

/* 只有盖子关着才闭锁，避免电机顶死 */
static void try_lock(pb_core_t *c, uint8_t slot)
{
    bool open = false;
    if (lid_open(c, slot, &open) && open) {
        return;
    }
    do_lock(c, slot);
}

static void set_state(pb_core_t *c, pb_state_t st, uint32_t now_ms)
{
    c->state = st;
    c->t_enter_ms = now_ms;
}

static uint8_t lowest_bit(uint32_t mask)
{
    for (uint8_t i = 0; i < PB_MAX_SLOTS; i++) {
        if ((mask & (1u << i)) != 0) {
            return i;
        }
    }
    return 0xFF;
}

/* ---------------- 电量与降级 ---------------- */

static void enter_storage(pb_core_t *c, uint32_t now_ms, uint8_t soc, uint32_t mv)
{
    if (c->storage_latched) {
        return;
    }
    c->storage_latched = true;
    c->battery_level = 3;
    c->pending_mask = 0;
    memset(c->pending_units, 0, sizeof(c->pending_units));
    c->retry = 0;

    /* 全部解锁：变回普通收纳盒 */
    for (uint8_t i = 0; i < c->slot_count; i++) {
        do_unlock(c, i);
        c->manual_active[i] = false;
    }
    set_state(c, PB_ST_STORAGE, now_ms);

    if (c->hal.display != NULL && c->hal.display->show_storage != NULL) {
        c->hal.display->show_storage(c->hal.display->ctx);
    }
    speak(c, PB_PHRASE_STORAGE, NULL);

    char ex[96];
    snprintf(ex, sizeof(ex), "\"soc\":%u,\"mv\":%lu,\"unlockedAll\":true",
             (unsigned)soc, (unsigned long)mv);
    emit(c, "storage_mode", ex);
    ble_alert(c, PB_ALERT_STORAGE, 0xFF);
}

static void poll_power(pb_core_t *c, uint32_t now_ms)
{
    if (c->hal.power == NULL) {
        return;
    }
    if (c->t_power_ms != 0 && (uint32_t)(now_ms - c->t_power_ms) < PB_POWER_PERIOD_MS) {
        return;
    }
    c->t_power_ms = now_ms;

    uint8_t  soc = 0;
    uint32_t mv = 0;
    const bool soc_ok = (c->hal.power->read_soc != NULL) &&
                        (c->hal.power->read_soc(c->hal.power->ctx, &soc) == PB_OK);
    const bool mv_ok = (c->hal.power->read_mv != NULL) &&
                       (c->hal.power->read_mv(c->hal.power->ctx, &mv) == PB_OK);
    if (!soc_ok && !mv_ok) {
        return;
    }

    const bool critical = (mv_ok && mv <= c->cfg.battery_cutoff_mv) ||
                          (soc_ok && soc <= c->cfg.battery_critical_pct);
    if (critical) {
        enter_storage(c, now_ms, soc, mv);
        return;
    }
    if (c->storage_latched) {
        return; /* 已进入收纳盒模式，不再回退 */
    }

    uint8_t level = 0;
    if (soc_ok && soc <= c->cfg.battery_low_pct) {
        level = 2;
    } else if (soc_ok && soc <= c->cfg.battery_warn_pct) {
        level = 1;
    }
    if (level > c->battery_level) {
        c->battery_level = level;
        char ex[80];
        snprintf(ex, sizeof(ex), "\"soc\":%u,\"mv\":%lu,\"level\":%u",
                 (unsigned)soc, (unsigned long)mv, (unsigned)level);
        emit(c, "low_battery", ex);
        speak(c, PB_PHRASE_LOW_BATTERY, NULL);
        ble_alert(c, PB_ALERT_LOW_BATTERY, 0xFF);
        if (c->hal.display != NULL && c->hal.display->show_message != NULL) {
            c->hal.display->show_message(c->hal.display->ctx, "电量低",
                                         level >= 2 ? "请尽快充电" : "请及时充电",
                                         "电量耗尽将自动解锁");
        }
    }
}

/* ---------------- 排程 ---------------- */

static void schedule_poll(pb_core_t *c)
{
    int64_t now = 0;
    if (!time_ok(c, &now)) {
        return;
    }
    const int32_t  day = pb_day_index(now, c->cfg.tz_offset_s);
    const uint16_t minute = pb_minute_of_day(now, c->cfg.tz_offset_s);

    for (uint8_t i = 0; i < c->slot_count; i++) {
        pb_slot_t *s = &c->slots[i];
        if (!s->used || s->dose_n == 0) {
            continue;
        }
        if (s->fired_day != day) {
            s->fired_day = day;
            s->fired_mask = 0;
        }
        for (uint8_t d = 0; d < s->dose_n && d < PB_MAX_DOSES_PER_DAY; d++) {
            if (!pb_dose_due(s, d, day, minute, c->cfg.due_grace_min)) {
                continue;
            }
            s->fired_mask |= (uint8_t)(1u << d);
            c->pending_units[i] = s->dose[d].units;
            c->pending_mask |= (1u << i);
        }
    }
}

/* 把某个药格切到"请取药"会话 */
static void start_due(pb_core_t *c, uint8_t slot, uint32_t now_ms)
{
    pb_slot_t *s = &c->slots[slot];
    const uint8_t units = (c->pending_units[slot] != 0) ? c->pending_units[slot] : 1;

    if (s->stock < units) {
        char name[PB_DRUG_NAME_LEN * 2];
        json_escape(name, sizeof(name), s->name);
        char ex[200];
        snprintf(ex, sizeof(ex),
                 "\"slot\":%u,\"drug\":\"%s\",\"stock\":%u,\"units\":%u",
                 (unsigned)(slot + 1), name, (unsigned)s->stock, (unsigned)units);
        emit(c, "stock_empty", ex);
        c->pending_mask &= ~(1u << slot);
        c->pending_units[slot] = 0;
        if (c->hal.display != NULL && c->hal.display->show_message != NULL) {
            c->hal.display->show_message(c->hal.display->ctx, "药格已空", s->name, "请联系家属补药");
        }
        return;
    }

    c->active_slot = slot;
    c->active_units = units;
    c->retry = 0;
    c->wrong_slot_reported = false;

    int32_t w = 0;
    if (read_weight(c, slot, &w)) {
        c->w_before_mg = w;
    } else {
        c->w_before_mg = s->empty_mg + (int32_t)s->stock * (int32_t)s->unit_mg;
    }

    bool open = false;
    (void)lid_open(c, slot, &open);
    c->lid_was_open = open;
    if (!open) {
        do_unlock(c, slot);
    }

    speak(c, PB_PHRASE_DUE, s->name);
    if (c->hal.display != NULL && c->hal.display->show_due != NULL) {
        c->hal.display->show_due(c->hal.display->ctx, (uint8_t)(slot + 1),
                                 s->name, units, s->stock);
    }

    char name[PB_DRUG_NAME_LEN * 2];
    json_escape(name, sizeof(name), s->name);
    char ex[220];
    snprintf(ex, sizeof(ex),
             "\"slot\":%u,\"drug\":\"%s\",\"units\":%u,\"stock\":%u,\"expectMg\":%ld",
             (unsigned)(slot + 1), name, (unsigned)units, (unsigned)s->stock,
             (long)((int32_t)units * (int32_t)s->unit_mg));
    emit(c, "dose_due", ex);
    ble_alert(c, PB_ALERT_DUE, slot);

    set_state(c, PB_ST_WAIT_TAKE, now_ms);
}

/* 升级告警：锁格 + 上报家属 + 停留 60s 后回到空闲 */
static void escalate(pb_core_t *c, const char *reason, uint32_t now_ms)
{
    const uint8_t slot = c->active_slot;
    pb_slot_t *s = &c->slots[slot];

    char name[PB_DRUG_NAME_LEN * 2];
    json_escape(name, sizeof(name), s->name);

    char ex[240];
    snprintf(ex, sizeof(ex),
             "\"slot\":%u,\"drug\":\"%s\",\"reason\":\"%s\",\"retry\":%u",
             (unsigned)(slot + 1), name, reason, (unsigned)c->retry);
    emit(c, "dose_escalate", ex);

    try_lock(c, slot);
    c->pending_mask &= ~(1u << slot);
    c->pending_units[slot] = 0;
    ble_alert(c, PB_ALERT_ESCALATE, slot);

    if (c->hal.display != NULL && c->hal.display->show_message != NULL) {
        c->hal.display->show_message(c->hal.display->ctx, "需要关注", s->name, "已通知家属");
    }
    set_state(c, PB_ST_ESCALATE, now_ms);
}

/* 取药窗口超时（没开盖 / 没取完就跑了） */
static void on_window_timeout(pb_core_t *c, uint32_t now_ms)
{
    pb_slot_t *s = &c->slots[c->active_slot];
    c->retry++;

    char name[PB_DRUG_NAME_LEN * 2];
    json_escape(name, sizeof(name), s->name);
    char ex[200];
    snprintf(ex, sizeof(ex),
             "\"slot\":%u,\"drug\":\"%s\",\"retry\":%u,\"windowS\":%u",
             (unsigned)(c->active_slot + 1), name, (unsigned)c->retry,
             (unsigned)c->cfg.take_window_s);
    emit(c, "dose_missed", ex);

    speak(c, PB_PHRASE_MISSED, s->name);
    ble_alert(c, PB_ALERT_MISSED, c->active_slot);
    if (c->hal.display != NULL && c->hal.display->show_message != NULL) {
        c->hal.display->show_message(c->hal.display->ctx, "未按时取药", s->name, "稍后再次提醒");
    }

    if (c->retry > c->cfg.max_retry) {
        escalate(c, "missed", now_ms);
        return;
    }
    set_state(c, PB_ST_MISSED, now_ms);
}

/* 失败重试 / 超时提醒后重新开格。
 * 注意：不刷新 w_before_mg —— 重试要按"本次会话累计减重"判定，
 * 否则分两次取够（每次只取一部分）永远判不过。 */
static void reopen_slot(pb_core_t *c, uint8_t slot, uint32_t now_ms)
{
    bool open = false;
    (void)lid_open(c, slot, &open);
    c->lid_was_open = open;
    if (!open) {
        do_unlock(c, slot);
    }
    set_state(c, PB_ST_WAIT_TAKE, now_ms);
}

/* 判定结果处理 */
static void handle_result(pb_core_t *c, const pb_dose_eval_t *ev, uint32_t now_ms)
{
    const uint8_t slot = c->active_slot;
    pb_slot_t *s = &c->slots[slot];

    char name[PB_DRUG_NAME_LEN * 2];
    json_escape(name, sizeof(name), s->name);

    char ex[280];
    snprintf(ex, sizeof(ex),
             "\"slot\":%u,\"drug\":\"%s\",\"units\":%u,\"expectMg\":%ld,\"actualMg\":%ld,"
             "\"remainMg\":%ld,\"residualMg\":%ld,\"retry\":%u,\"result\":\"%s\"",
             (unsigned)(slot + 1), name, (unsigned)c->active_units,
             (long)ev->expect_mg, (long)ev->actual_mg,
             (long)ev->remain_mg, (long)ev->residual_mg,
             (unsigned)c->retry, pb_dose_result_name(ev->result));

    if (ev->result == PB_DOSE_OK || ev->result == PB_DOSE_OVER) {
        if (ev->result == PB_DOSE_OK) {
            pb_stock_consume(s, c->active_units);
        } else {
            /* 多取：按实测减重折算片数扣库存 */
            int32_t taken = c->active_units;
            if (s->unit_mg != 0 && ev->actual_mg > 0) {
                taken = (ev->actual_mg + (int32_t)s->unit_mg / 2) / (int32_t)s->unit_mg;
            }
            if (taken < 1) {
                taken = 1;
            }
            pb_stock_consume(s, (uint16_t)taken);
        }
        c->pending_mask &= ~(1u << slot);
        c->pending_units[slot] = 0;
        c->retry = 0;
        try_lock(c, slot);
        emit(c, (ev->result == PB_DOSE_OK) ? "dose_ok" : "dose_over", ex);
        speak(c, PB_PHRASE_DOSE_OK, s->name);
        ble_alert(c, PB_ALERT_OK, slot);
        if (c->hal.display != NULL && c->hal.display->show_message != NULL) {
            c->hal.display->show_message(c->hal.display->ctx, "取药完成", s->name,
                                         (ev->result == PB_DOSE_OK) ? "已记录" : "注意：用量偏多");
        }
        set_state(c, PB_ST_IDLE, now_ms);
        return;
    }

    /* 失败：NONE / PARTIAL */
    c->retry++;
    if (ev->result == PB_DOSE_NONE) {
        emit(c, "dose_none", ex);
        speak(c, PB_PHRASE_MISSED, s->name);
        ble_alert(c, PB_ALERT_MISSED, slot);
        if (c->hal.display != NULL && c->hal.display->show_message != NULL) {
            c->hal.display->show_message(c->hal.display->ctx, "未检测到取药", s->name, "请重新取药");
        }
    } else {
        emit(c, "dose_partial", ex);
        speak(c, PB_PHRASE_NOT_FINISHED, s->name);
        ble_alert(c, PB_ALERT_NOT_FINISHED, slot);
        if (c->hal.display != NULL && c->hal.display->show_message != NULL) {
            c->hal.display->show_message(c->hal.display->ctx, "药片未取完", s->name, "请把药取完");
        }
    }

    if (c->retry > c->cfg.max_retry) {
        escalate(c, (ev->result == PB_DOSE_NONE) ? "not_taken" : "not_finished", now_ms);
        return;
    }

    /* 重新开格再给一次机会（重新取基准重量） */
    reopen_slot(c, slot, now_ms);
}

/* ---------------- 各状态处理 ---------------- */

static void tick_init(pb_core_t *c, uint32_t now_ms)
{
    /* 上电先全部闭锁，建立已知机械状态（电量临界时已被 poll_power 拦到 STORAGE） */
    for (uint8_t i = 0; i < c->slot_count; i++) {
        c->locked[i] = false;
        try_lock(c, i);
    }
    set_state(c, PB_ST_IDLE, now_ms);
}

static void tick_wait_take(pb_core_t *c, uint32_t now_ms)
{
    const uint8_t slot = c->active_slot;
    const pb_slot_t *s = &c->slots[slot];

    bool open = false;
    (void)lid_open(c, slot, &open);

    if (open && !c->lid_was_open) {
        c->lid_was_open = true;
        char name[PB_DRUG_NAME_LEN * 2];
        json_escape(name, sizeof(name), s->name);
        char ex[160];
        snprintf(ex, sizeof(ex), "\"slot\":%u,\"drug\":\"%s\"",
                 (unsigned)(slot + 1), name);
        emit(c, "lid_open", ex);
        set_state(c, PB_ST_TAKING, now_ms);
        return;
    }

    /* 开错格：用户打开了别的格子 */
    if (!c->wrong_slot_reported) {
        for (uint8_t i = 0; i < c->slot_count; i++) {
            if (i == slot) {
                continue;
            }
            bool other = false;
            if (lid_open(c, i, &other) && other) {
                char name[PB_DRUG_NAME_LEN * 2];
                json_escape(name, sizeof(name), s->name);
                char ex[176];
                snprintf(ex, sizeof(ex),
                         "\"expectSlot\":%u,\"openedSlot\":%u,\"drug\":\"%s\"",
                         (unsigned)(slot + 1), (unsigned)(i + 1), name);
                emit(c, "wrong_slot", ex);
                speak(c, PB_PHRASE_WRONG_SLOT, s->name);
                ble_alert(c, PB_ALERT_NOT_FINISHED, slot);
                c->wrong_slot_reported = true;
                break;
            }
        }
    }

    const uint32_t window_s = (s->take_window_s != 0) ? s->take_window_s : c->cfg.take_window_s;
    if ((uint32_t)(now_ms - c->t_enter_ms) >= window_s * 1000u) {
        on_window_timeout(c, now_ms);
    }
}

static void tick_taking(pb_core_t *c, uint32_t now_ms)
{
    const uint8_t slot = c->active_slot;
    const pb_slot_t *s = &c->slots[slot];

    bool open = false;
    (void)lid_open(c, slot, &open);

    if (!open) {
        c->lid_was_open = false;
        c->t_lid_close_ms = now_ms;
        set_state(c, PB_ST_VERIFY, now_ms);
        return;
    }

    const uint32_t window_s = (s->take_window_s != 0) ? s->take_window_s : c->cfg.take_window_s;
    if ((uint32_t)(now_ms - c->t_enter_ms) >= window_s * 2000u) {
        /* 盖子长时间没关好：强制进入判定，避免流程卡死 */
        c->t_lid_close_ms = now_ms;
        set_state(c, PB_ST_VERIFY, now_ms);
    }
}

static void tick_verify(pb_core_t *c, uint32_t now_ms)
{
    const uint32_t since_close = (uint32_t)(now_ms - c->t_lid_close_ms);
    if (since_close < (uint32_t)c->cfg.settle_ms) {
        return; /* 等重量稳定 */
    }

    int32_t w_after = 0;
    if (!read_weight(c, c->active_slot, &w_after)) {
        if (since_close > (uint32_t)c->cfg.settle_ms + (uint32_t)c->cfg.verify_timeout_ms) {
            emit(c, "fault", "\"reason\":\"weight_read\"");
            c->active_slot = 0xFF;
            set_state(c, PB_ST_FAULT, now_ms);
        }
        return;
    }

    pb_dose_eval_t ev;
    pb_evaluate_dose(&c->slots[c->active_slot], c->active_units,
                     c->w_before_mg, w_after, c->cfg.noise_mg, &ev);
    handle_result(c, &ev, now_ms);
}

static void tick_missed(pb_core_t *c, uint32_t now_ms)
{
    if ((uint32_t)(now_ms - c->t_enter_ms) < (uint32_t)c->cfg.remind_interval_s * 1000u) {
        return;
    }

    const uint8_t slot = c->active_slot;
    const pb_slot_t *s = &c->slots[slot];

    char name[PB_DRUG_NAME_LEN * 2];
    json_escape(name, sizeof(name), s->name);
    char ex[160];
    snprintf(ex, sizeof(ex), "\"slot\":%u,\"drug\":\"%s\",\"retry\":%u",
             (unsigned)(slot + 1), name, (unsigned)c->retry);
    emit(c, "dose_remind", ex);

    speak(c, PB_PHRASE_MISSED, s->name);
    ble_alert(c, PB_ALERT_MISSED, slot);

    reopen_slot(c, slot, now_ms);
}

/* 空闲时把该锁的格子锁回去（含"开盖后延迟闭锁"和"远程开格超时回锁"） */
static void idle_relock(pb_core_t *c, uint32_t now_ms)
{
    if (c->storage_latched || c->state == PB_ST_STORAGE || c->state == PB_ST_FAULT) {
        return;
    }
    for (uint8_t i = 0; i < c->slot_count; i++) {
        if (c->manual_active[i]) {
            if ((int32_t)(now_ms - c->manual_until_ms[i]) < 0) {
                continue; /* 还没到回锁时间 */
            }
            c->manual_active[i] = false;
            char ex[72];
            snprintf(ex, sizeof(ex), "\"slot\":%u,\"reason\":\"remote_timeout\"",
                     (unsigned)(i + 1));
            emit(c, "auto_lock", ex);
        }
        if ((c->pending_mask & (1u << i)) != 0) {
            continue;
        }
        if (c->locked[i]) {
            continue;
        }
        bool open = false;
        if (lid_open(c, i, &open) && open) {
            continue;
        }
        do_lock(c, i);
    }
}

static void tick_idle(pb_core_t *c, uint32_t now_ms)
{
    idle_relock(c, now_ms);

    if (c->pending_mask != 0) {
        const uint8_t slot = lowest_bit(c->pending_mask);
        if (slot != 0xFF) {
            start_due(c, slot, now_ms);
        }
        return;
    }

    if ((uint32_t)(now_ms - c->t_display_ms) >= PB_IDLE_REFRESH_MS) {
        c->t_display_ms = now_ms;
        int64_t now = 0;
        if (time_ok(c, &now) && c->hal.display != NULL && c->hal.display->show_idle != NULL) {
            int32_t slot = -1;
            const int32_t sec = pb_seconds_to_next(c->slots, c->slot_count, now,
                                                   c->cfg.tz_offset_s, &slot);
            c->hal.display->show_idle(c->hal.display->ctx, sec,
                                      (slot < 0) ? -1 : (int32_t)slot + 1);
        }
    }
}

/* ---------------- 主循环入口 ---------------- */

void pb_core_tick(pb_core_t *c, uint32_t now_ms)
{
    if (c == NULL) {
        return;
    }
    c->now_ms = now_ms;

    poll_power(c, now_ms);

    if (c->state != PB_ST_STORAGE && c->state != PB_ST_FAULT) {
        schedule_poll(c);
    }

    switch (c->state) {
    case PB_ST_INIT:
        tick_init(c, now_ms);
        break;
    case PB_ST_IDLE:
        tick_idle(c, now_ms);
        break;
    case PB_ST_WAIT_TAKE:
        tick_wait_take(c, now_ms);
        break;
    case PB_ST_TAKING:
        tick_taking(c, now_ms);
        break;
    case PB_ST_VERIFY:
        tick_verify(c, now_ms);
        break;
    case PB_ST_MISSED:
        tick_missed(c, now_ms);
        break;
    case PB_ST_ESCALATE:
        idle_relock(c, now_ms);
        if ((uint32_t)(now_ms - c->t_enter_ms) >= PB_ESCALATE_HOLD_MS) {
            c->active_slot = 0xFF;
            set_state(c, PB_ST_IDLE, now_ms);
        }
        break;
    case PB_ST_DUE:
    case PB_ST_PARTIAL:
    case PB_ST_STORAGE:
    case PB_ST_FAULT:
    default:
        break;
    }
}

/* ---------------- 对外 API ---------------- */

pb_config_t pb_default_config(void)
{
    pb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.slot_count           = 6;
    cfg.noise_mg             = 150;
    cfg.settle_ms            = 3000;
    cfg.verify_timeout_ms    = 2000;
    cfg.take_window_s        = 900;
    cfg.due_grace_min        = 10;
    cfg.remind_interval_s    = 300;
    cfg.max_retry            = 2;
    cfg.battery_warn_pct     = 20;
    cfg.battery_low_pct      = 10;
    cfg.battery_critical_pct = 5;
    cfg.battery_cutoff_mv    = 3450;
    cfg.tz_offset_s          = 8 * 3600;
    return cfg;
}

pb_core_t *pb_core_create(const pb_hal_t *hal, const pb_config_t *cfg)
{
    if (hal == NULL) {
        return NULL;
    }
    pb_core_t *c = (pb_core_t *)calloc(1, sizeof(pb_core_t));
    if (c == NULL) {
        return NULL;
    }
    c->hal = *hal;
    c->cfg = (cfg != NULL) ? *cfg : pb_default_config();

    /* 兜底：0 值参数回落到默认值，避免"永远不触发/永远等待" */
    const pb_config_t def = pb_default_config();
    if (c->cfg.slot_count == 0 || c->cfg.slot_count > PB_MAX_SLOTS) {
        c->cfg.slot_count = def.slot_count;
    }
    if (c->cfg.noise_mg == 0)             c->cfg.noise_mg = def.noise_mg;
    if (c->cfg.settle_ms == 0)            c->cfg.settle_ms = def.settle_ms;
    if (c->cfg.verify_timeout_ms == 0)    c->cfg.verify_timeout_ms = def.verify_timeout_ms;
    if (c->cfg.take_window_s == 0)        c->cfg.take_window_s = def.take_window_s;
    if (c->cfg.due_grace_min == 0)        c->cfg.due_grace_min = def.due_grace_min;
    if (c->cfg.remind_interval_s == 0)    c->cfg.remind_interval_s = def.remind_interval_s;
    if (c->cfg.max_retry == 0)            c->cfg.max_retry = def.max_retry;
    if (c->cfg.battery_warn_pct == 0)     c->cfg.battery_warn_pct = def.battery_warn_pct;
    if (c->cfg.battery_low_pct == 0)      c->cfg.battery_low_pct = def.battery_low_pct;
    if (c->cfg.battery_critical_pct == 0) c->cfg.battery_critical_pct = def.battery_critical_pct;
    if (c->cfg.battery_cutoff_mv == 0)    c->cfg.battery_cutoff_mv = def.battery_cutoff_mv;

    c->slot_count = c->cfg.slot_count;
    c->state = PB_ST_INIT;
    c->active_slot = 0xFF;
    return c;
}

void pb_core_destroy(pb_core_t *c)
{
    free(c);
}

pb_err_t pb_core_set_slots(pb_core_t *c, const pb_slot_t *slots, uint8_t n)
{
    if (c == NULL || slots == NULL || n == 0 || n > PB_MAX_SLOTS) {
        return PB_ERR_INVALID;
    }
    for (uint8_t i = 0; i < n; i++) {
        if (!pb_slot_validate(&slots[i])) {
            return PB_ERR_INVALID;
        }
    }
    /* 会话进行中不允许换药单，避免状态错乱 */
    if (c->active_slot != 0xFF && c->state != PB_ST_IDLE && c->state != PB_ST_ESCALATE) {
        return PB_ERR_BUSY;
    }

    memset(c->slots, 0, sizeof(c->slots));
    for (uint8_t i = 0; i < n; i++) {
        c->slots[i] = slots[i];
        c->slots[i].fired_day = 0;
        c->slots[i].fired_mask = 0;
    }
    c->slot_count = n;
    c->cfg.slot_count = n;
    c->pending_mask = 0;
    memset(c->pending_units, 0, sizeof(c->pending_units));
    c->storage_latched = false;
    c->battery_level = 0;
    c->active_slot = 0xFF;
    set_state(c, PB_ST_IDLE, c->now_ms);
    return PB_OK;
}

pb_state_t pb_core_state(const pb_core_t *c)
{
    return (c != NULL) ? c->state : PB_ST_INIT;
}

uint8_t pb_core_active_slot(const pb_core_t *c)
{
    return (c != NULL) ? c->active_slot : 0xFF;
}

uint8_t pb_core_slot_count(const pb_core_t *c)
{
    return (c != NULL) ? c->slot_count : 0;
}

const pb_slot_t *pb_core_slot(const pb_core_t *c, uint8_t idx)
{
    if (c == NULL || idx >= c->slot_count) {
        return NULL;
    }
    return &c->slots[idx];
}

bool pb_core_is_storage_mode(const pb_core_t *c)
{
    return (c != NULL) && c->storage_latched;
}

int32_t pb_core_seconds_to_next_dose(const pb_core_t *c, int64_t now_s, int32_t *slot_out)
{
    if (c == NULL) {
        if (slot_out != NULL) {
            *slot_out = -1;
        }
        return -1;
    }
    return pb_seconds_to_next(c->slots, c->slot_count, now_s, c->cfg.tz_offset_s, slot_out);
}

/* ---------------- 命令 ---------------- */

void pb_core_cmd_unlock_slot(pb_core_t *c, uint8_t slot, uint32_t now_ms)
{
    if (c == NULL || slot >= c->slot_count) {
        return;
    }
    do_unlock(c, slot);
    c->manual_active[slot] = true;
    c->manual_until_ms[slot] = now_ms + PB_REMOTE_UNLOCK_MS;

    char ex[96];
    snprintf(ex, sizeof(ex), "\"slot\":%u,\"ttlS\":%u",
             (unsigned)(slot + 1), (unsigned)(PB_REMOTE_UNLOCK_MS / 1000u));
    emit(c, "remote_unlock", ex);
}

void pb_core_cmd_unlock_all(pb_core_t *c, uint32_t now_ms)
{
    if (c == NULL) {
        return;
    }
    for (uint8_t i = 0; i < c->slot_count; i++) {
        do_unlock(c, i);
        c->manual_active[i] = true;
        c->manual_until_ms[i] = now_ms + PB_REMOTE_UNLOCK_MS;
    }
    emit(c, "remote_unlock", "\"slot\":0,\"all\":true");
}

void pb_core_cmd_ack(pb_core_t *c, uint32_t now_ms)
{
    if (c == NULL) {
        return;
    }
    if (c->hal.audio != NULL && c->hal.audio->stop != NULL) {
        c->hal.audio->stop(c->hal.audio->ctx);
    }
    if (c->state == PB_ST_ESCALATE) {
        c->active_slot = 0xFF;
        set_state(c, PB_ST_IDLE, now_ms);
        emit(c, "ack", "\"state\":\"escalate\"");
    }
}

void pb_core_cmd_storage(pb_core_t *c, uint32_t now_ms)
{
    if (c == NULL) {
        return;
    }
    enter_storage(c, now_ms, 0, 0);
}

void pb_core_cmd_silence(pb_core_t *c)
{
    if (c == NULL) {
        return;
    }
    if (c->hal.audio != NULL && c->hal.audio->stop != NULL) {
        c->hal.audio->stop(c->hal.audio->ctx);
    }
    emit(c, "silence", NULL);
}

void pb_core_cmd_recheck_battery(pb_core_t *c, uint32_t now_ms)
{
    if (c == NULL) {
        return;
    }
    c->t_power_ms = 0;
    poll_power(c, now_ms);
}

bool pb_core_on_button(pb_core_t *c, uint8_t key_id, uint32_t now_ms)
{
    if (c == NULL) {
        return false;
    }
    switch (key_id) {
    case 0: /* 确认键 */
        pb_core_cmd_ack(c, now_ms);
        return true;
    case 1: /* 静音键 */
        pb_core_cmd_silence(c);
        return true;
    default:
        return false;
    }
}






