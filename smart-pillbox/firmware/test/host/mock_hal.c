#include "mock_hal.h"

#include <stdio.h>
#include <string.h>

/* ---------------- 各能力实现 ---------------- */

static pb_err_t w_init(void *ctx, uint8_t slot)
{
    (void)ctx;
    (void)slot;
    return PB_OK;
}

static pb_err_t w_tare(void *ctx, uint8_t slot)
{
    (void)ctx;
    (void)slot;
    return PB_OK;
}

static pb_err_t w_read(void *ctx, uint8_t slot, int32_t *mg)
{
    mock_t *m = (mock_t *)ctx;
    if (slot >= PB_MAX_SLOTS || mg == NULL) {
        return PB_ERR_INVALID;
    }
    if (m->weight_fail[slot]) {
        return PB_ERR_IO;
    }
    *mg = m->weight_mg[slot];
    return PB_OK;
}

static pb_err_t w_cal(void *ctx, uint8_t slot, int32_t known_mg)
{
    (void)ctx;
    (void)slot;
    (void)known_mg;
    return PB_OK;
}

static pb_err_t l_init(void *ctx, uint8_t slot)
{
    mock_t *m = (mock_t *)ctx;
    if (slot < PB_MAX_SLOTS) {
        m->locked[slot] = true;
    }
    return PB_OK;
}

static pb_err_t l_lock(void *ctx, uint8_t slot)
{
    mock_t *m = (mock_t *)ctx;
    if (slot >= PB_MAX_SLOTS) {
        return PB_ERR_INVALID;
    }
    m->locked[slot] = true;
    m->lock_cnt[slot]++;
    return PB_OK;
}

static pb_err_t l_unlock(void *ctx, uint8_t slot)
{
    mock_t *m = (mock_t *)ctx;
    if (slot >= PB_MAX_SLOTS) {
        return PB_ERR_INVALID;
    }
    m->locked[slot] = false;
    m->unlock_cnt[slot]++;
    return PB_OK;
}

static pb_err_t l_state(void *ctx, uint8_t slot, bool *locked)
{
    mock_t *m = (mock_t *)ctx;
    if (slot >= PB_MAX_SLOTS || locked == NULL) {
        return PB_ERR_INVALID;
    }
    *locked = m->locked[slot];
    return PB_OK;
}

static pb_err_t lid_is_open(void *ctx, uint8_t slot, bool *open)
{
    mock_t *m = (mock_t *)ctx;
    if (slot >= PB_MAX_SLOTS || open == NULL) {
        return PB_ERR_INVALID;
    }
    *open = m->lid[slot];
    return PB_OK;
}

static pb_err_t a_speak(void *ctx, uint16_t phrase, const char *text)
{
    mock_t *m = (mock_t *)ctx;
    m->speak_cnt++;
    m->last_phrase = phrase;
    if (text != NULL) {
        snprintf(m->last_text, sizeof(m->last_text), "%s", text);
    } else {
        m->last_text[0] = '\0';
    }
    return PB_OK;
}

static pb_err_t a_stop(void *ctx)
{
    mock_t *m = (mock_t *)ctx;
    m->speak_stop_cnt++;
    return PB_OK;
}

static pb_err_t d_show_due(void *ctx, uint8_t slot, const char *drug,
                           uint8_t units, uint16_t stock)
{
    mock_t *m = (mock_t *)ctx;
    (void)slot;
    (void)drug;
    (void)units;
    (void)stock;
    m->disp_due_cnt++;
    return PB_OK;
}

static pb_err_t d_show_idle(void *ctx, int32_t sec, int32_t slot)
{
    mock_t *m = (mock_t *)ctx;
    (void)sec;
    (void)slot;
    m->disp_idle_cnt++;
    return PB_OK;
}

static pb_err_t d_show_msg(void *ctx, const char *title, const char *l1, const char *l2)
{
    mock_t *m = (mock_t *)ctx;
    (void)title;
    (void)l1;
    (void)l2;
    m->disp_msg_cnt++;
    return PB_OK;
}

static pb_err_t d_show_storage(void *ctx)
{
    mock_t *m = (mock_t *)ctx;
    m->disp_storage_cnt++;
    return PB_OK;
}

static pb_err_t d_backlight(void *ctx, bool on)
{
    (void)ctx;
    (void)on;
    return PB_OK;
}

static pb_err_t p_init(void *ctx)
{
    (void)ctx;
    return PB_OK;
}

static pb_err_t p_soc(void *ctx, uint8_t *pct)
{
    mock_t *m = (mock_t *)ctx;
    if (m->power_fail || pct == NULL) {
        return PB_ERR_IO;
    }
    *pct = m->soc;
    return PB_OK;
}

static pb_err_t p_mv(void *ctx, uint32_t *mv)
{
    mock_t *m = (mock_t *)ctx;
    if (m->power_fail || mv == NULL) {
        return PB_ERR_IO;
    }
    *mv = m->mv;
    return PB_OK;
}

static pb_err_t n_publish(void *ctx, const char *json)
{
    mock_t *m = (mock_t *)ctx;
    if (json == NULL) {
        return PB_ERR_INVALID;
    }
    if (m->event_cnt < MOCK_MAX_EVENTS) {
        snprintf(m->events[m->event_cnt], sizeof(m->events[0]), "%s", json);
        m->event_cnt++;
    }
    return PB_OK;
}

static pb_err_t n_bracelet(void *ctx, uint8_t alert, uint8_t slot)
{
    mock_t *m = (mock_t *)ctx;
    m->ble_cnt++;
    m->last_alert = alert;
    m->last_ble_slot = slot;
    return PB_OK;
}

static pb_err_t t_unix(void *ctx, int64_t *sec)
{
    mock_t *m = (mock_t *)ctx;
    if (sec == NULL) {
        return PB_ERR_INVALID;
    }
    *sec = m->now_s;
    return PB_OK;
}

static bool t_valid(void *ctx)
{
    mock_t *m = (mock_t *)ctx;
    return m->time_valid;
}

/* ---------------- 组装 ---------------- */

void mock_init(mock_t *m)
{
    memset(m, 0, sizeof(*m));
    m->soc = 80;
    m->mv = 4000;
    m->time_valid = true;
    m->now_s = (int64_t)20000 * 86400;
    for (int i = 0; i < PB_MAX_SLOTS; i++) {
        m->locked[i] = true;
    }
}

pb_hal_t mock_hal(mock_t *m)
{
    /* 每个测试只用一个 mock，所以 vtable 用静态可变副本即可 */
    static pb_weight_ops_t  w;
    static pb_lock_ops_t    l;
    static pb_lid_ops_t     ld;
    static pb_audio_ops_t   a;
    static pb_display_ops_t d;
    static pb_power_ops_t   p;
    static pb_notify_ops_t  n;
    static pb_time_ops_t    t;

    w.init = w_init;  w.tare = w_tare;  w.read_mg = w_read;  w.calibrate = w_cal;  w.ctx = m;
    l.init = l_init;  l.lock = l_lock;  l.unlock = l_unlock;  l.get_state = l_state; l.ctx = m;
    ld.is_open = lid_is_open; ld.ctx = m;
    a.speak = a_speak; a.stop = a_stop; a.ctx = m;
    d.show_due = d_show_due; d.show_idle = d_show_idle; d.show_message = d_show_msg;
    d.show_storage = d_show_storage; d.backlight = d_backlight; d.ctx = m;
    p.init = p_init; p.read_mv = p_mv; p.read_soc = p_soc; p.ctx = m;
    n.publish_event = n_publish; n.notify_bracelet = n_bracelet; n.ctx = m;
    t.unix_time = t_unix; t.time_valid = t_valid; t.ctx = m;

    pb_hal_t h;
    h.weight = &w;
    h.lock = &l;
    h.lid = &ld;
    h.audio = &a;
    h.display = &d;
    h.power = &p;
    h.notify = &n;
    h.time = &t;
    return h;
}

void mock_set_time(mock_t *m, int day, int hour, int minute, int second)
{
    m->now_s = (int64_t)day * 86400 + hour * 3600 + minute * 60 + second;
}

int mock_event_count(const mock_t *m, const char *type)
{
    char needle[64];
    int  cnt = 0;
    snprintf(needle, sizeof(needle), "\"type\":\"%s\"", type);
    for (int i = 0; i < m->event_cnt; i++) {
        if (strstr(m->events[i], needle) != NULL) {
            cnt++;
        }
    }
    return cnt;
}

const char *mock_last_event(const mock_t *m, const char *type)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"type\":\"%s\"", type);
    for (int i = m->event_cnt - 1; i >= 0; i--) {
        if (strstr(m->events[i], needle) != NULL) {
            return m->events[i];
        }
    }
    return NULL;
}

bool mock_event_contains(const mock_t *m, const char *type, const char *needle)
{
    const char *ev = mock_last_event(m, type);
    return (ev != NULL) && (strstr(ev, needle) != NULL);
}

