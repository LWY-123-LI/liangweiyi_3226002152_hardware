/*
 * hal_esp.c —— pb_hal_t 的 ESP32-S3 实现
 *
 *   weight  : HX711 × 8（共用 SCK），24bit 补码读取 + 中值滤波 + 标定
 *   lock    : 2 × 74HC595 (16bit) 驱动 8 路 H 桥，脉动后断电由蜗轮自锁
 *   lid     : 2 × 74HC165 (16bit) 读 8 路开盖 + 8 路锁到位
 *   audio   : WT588D 串口语音模块（UART1）
 *   display : ST7789 状态色 + 三级状态灯（文字渲染见 ui 层扩展点）
 *   power   : MAX17048（I2C）+ 电压兜底
 *   time    : DS3231（I2C），由 APP/MQTT 校时
 *   notify  : MQTT 优先，断网进 NVS 离线队列；同时尝试 BLE 手环
 */
#include "board_pins.h"
#include "hal_esp.h"
#include "hal_if.h"

#include "ble.h"
#include "net.h"
#include "offline_queue.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "hal";

static const gpio_num_t PIN_HX711_DOUT[HX711_SLOT_NUM] = {
    PIN_HX711_DOUT_0, PIN_HX711_DOUT_1, PIN_HX711_DOUT_2, PIN_HX711_DOUT_3,
    PIN_HX711_DOUT_4, PIN_HX711_DOUT_5, PIN_HX711_DOUT_6, PIN_HX711_DOUT_7,
};

#define HX711_READY_TIMEOUT_US  120000  /* 未接传感器时 DOUT 一直高 */
#define HX711_FILTER_N          9
#define LID_OPEN_LEVEL          1       /* 开盖时 74HC165 对应位为高 */
#define LOCK_SENSE_LEVEL        1       /* 锁舌到位时为高 */

static TaskHandle_t       s_lock_task;
static QueueHandle_t      s_lock_q;
static volatile uint8_t   s_unlocked_mask;   /* 1 = 已开（软件状态） */
static bool               s_has_165 = true;

static int32_t  s_offset[HX711_SLOT_NUM];    /* 空盒零点（raw） */
static int32_t  s_scale_x1000[HX711_SLOT_NUM]; /* mg/count ×1000 */
static int32_t  s_tz_offset_s = 8 * 3600;

static esp_lcd_panel_handle_t s_panel;
static int32_t  s_bl_duty;                   /* 背光 0..1023 */

static void lcd_init(void); /* 定义在文件末尾，供 hal_esp_init 调用 */

/* ---------------------------------------------------------------- 通用 IO */

static void gpio_out(gpio_num_t pin)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static void gpio_in(gpio_num_t pin, gpio_pullup_t pu)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pu,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

/* ---------------------------------------------------------------- HX711 */

static int32_t hx711_raw(uint8_t slot)
{
    const gpio_num_t dout = PIN_HX711_DOUT[slot];
    const int64_t t0 = esp_timer_get_time();

    while (gpio_get_level(dout) != 0) {
        if (esp_timer_get_time() - t0 > HX711_READY_TIMEOUT_US) {
            return INT32_MIN; /* 超时：模块未接或损坏 */
        }
        vTaskDelay(1);
    }

    int32_t v = 0;
    for (int i = 0; i < 24; ++i) {
        gpio_set_level(PIN_HX711_SCK, 1);
        esp_rom_delay_us(1);
        v = (v << 1) | (gpio_get_level(dout) & 1);
        gpio_set_level(PIN_HX711_SCK, 0);
        esp_rom_delay_us(1);
    }
    /* 第 25 个脉冲：下次仍为通道 A、增益 128 */
    gpio_set_level(PIN_HX711_SCK, 1);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_HX711_SCK, 0);

    if (v & 0x800000) {
        v |= (int32_t)0xFF000000; /* 符号扩展 */
    }
    return v;
}

static void isort(int32_t *a, int n)
{
    for (int i = 1; i < n; ++i) {
        const int32_t k = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > k) {
            a[j + 1] = a[j];
            --j;
        }
        a[j + 1] = k;
    }
}

/* 中值滤波，抑制 HX711 偶发跳变 */
static int32_t hx711_filtered(uint8_t slot)
{
    int32_t buf[HX711_FILTER_N];
    int n = 0;
    for (int i = 0; i < HX711_FILTER_N; ++i) {
        const int32_t v = hx711_raw(slot);
        if (v != INT32_MIN) {
            buf[n++] = v;
        }
    }
    if (n == 0) {
        return INT32_MIN;
    }
    isort(buf, n);
    return buf[n / 2];
}

/* ---------------------------------------------------------------- 称重 ops */

static pb_err_t w_init(void *ctx, uint8_t slot)
{
    (void)ctx;
    if (slot >= HX711_SLOT_NUM) {
        return PB_ERR_INVALID;
    }
    gpio_in(PIN_HX711_DOUT[slot], GPIO_PULLUP_ENABLE);
    if (s_scale_x1000[slot] == 0) {
        s_scale_x1000[slot] = 1000; /* 1 count = 1 mg，仅占位，必须现场标定 */
    }
    return PB_OK;
}

static pb_err_t w_tare(void *ctx, uint8_t slot)
{
    (void)ctx;
    if (slot >= HX711_SLOT_NUM) {
        return PB_ERR_INVALID;
    }
    const int32_t raw = hx711_filtered(slot);
    if (raw == INT32_MIN) {
        return PB_ERR_TIMEOUT;
    }
    s_offset[slot] = raw;
    ESP_LOGI(TAG, "slot %u tared, offset=%ld", slot, (long)raw);
    return PB_OK;
}

static pb_err_t w_read(void *ctx, uint8_t slot, int32_t *mg)
{
    (void)ctx;
    if (slot >= HX711_SLOT_NUM || mg == NULL) {
        return PB_ERR_INVALID;
    }
    const int32_t raw = hx711_filtered(slot);
    if (raw == INT32_MIN) {
        return PB_ERR_TIMEOUT;
    }
    /* 已去皮：返回相对空盒的重量，因此上层 slot.empty_mg 应为 0 */
    const int64_t delta = (int64_t)(raw - s_offset[slot]);
    *mg = (int32_t)(delta * s_scale_x1000[slot] / 1000);
    return PB_OK;
}

static pb_err_t w_calibrate(void *ctx, uint8_t slot, int32_t known_mg)
{
    (void)ctx;
    if (slot >= HX711_SLOT_NUM || known_mg <= 0) {
        return PB_ERR_INVALID;
    }
    const int32_t raw = hx711_filtered(slot);
    if (raw == INT32_MIN) {
        return PB_ERR_TIMEOUT;
    }
    const int32_t delta = raw - s_offset[slot];
    if (delta == 0) {
        return PB_ERR_INVALID;
    }
    s_scale_x1000[slot] = (int32_t)(((int64_t)known_mg * 1000) / delta);
    ESP_LOGI(TAG, "slot %u calibrated: %ld mg -> %ld counts, scale=%ld",
             slot, (long)known_mg, (long)delta, (long)s_scale_x1000[slot]);
    return PB_OK;
}

static const pb_weight_ops_t s_weight_ops = {
    .init = w_init, .tare = w_tare, .read_mg = w_read, .calibrate = w_calibrate,
};

/* ------------------------------------------------------- 74HC595 / 74HC165 */

static void sh595_write(uint16_t bits)
{
    for (int i = 15; i >= 0; --i) {
        gpio_set_level(PIN_SH595_DATA, (bits >> i) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_SH595_CLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_SH595_CLK, 0);
    }
    gpio_set_level(PIN_SH595_LATCH, 1);
    esp_rom_delay_us(2);
    gpio_set_level(PIN_SH595_LATCH, 0);
}

static uint16_t sh165_read(void)
{
    uint16_t v = 0;
    gpio_set_level(PIN_SH165_LOAD, 0);
    esp_rom_delay_us(2);
    gpio_set_level(PIN_SH165_LOAD, 1);
    for (int i = 0; i < 16; ++i) {
        v = (uint16_t)((v << 1) | (gpio_get_level(PIN_SH165_DATA) & 1));
        gpio_set_level(PIN_SH165_CLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_SH165_CLK, 0);
        esp_rom_delay_us(1);
    }
    return v;
}

/* ---------------------------------------------------------------- 锁 ops */

typedef struct {
    uint8_t slot;
    bool    unlock;
} lock_req_t;

static void lock_motor_drive(uint8_t slot, bool unlock, bool on)
{
    /* 每格 2 bit：IN1 = 2*slot，IN2 = 2*slot+1；正反转各走一个方向 */
    uint16_t bits = 0;
    if (on) {
        bits = unlock ? (uint16_t)(1u << (2u * slot)) : (uint16_t)(1u << (2u * slot + 1u));
    }
    sh595_write(bits);

    /* 总使能：动作时给满占空，停时关断，避免堵转发热 */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 1023 : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void lock_task(void *arg)
{
    (void)arg;
    lock_req_t req;
    for (;;) {
        if (xQueueReceive(s_lock_q, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (req.slot >= HX711_SLOT_NUM) {
            continue;
        }
        lock_motor_drive(req.slot, req.unlock, true);
        vTaskDelay(pdMS_TO_TICKS(LOCK_PULSE_MS));
        lock_motor_drive(req.slot, req.unlock, false);

        if (req.unlock) {
            s_unlocked_mask |= (uint8_t)(1u << req.slot);
        } else {
            s_unlocked_mask &= (uint8_t)~(1u << req.slot);
        }
        ESP_LOGI(TAG, "slot %u %s", req.slot, req.unlock ? "unlocked" : "locked");
    }
}

static pb_err_t lock_act(uint8_t slot, bool unlock)
{
    if (slot >= HX711_SLOT_NUM) {
        return PB_ERR_INVALID;
    }
    if (s_lock_q == NULL) {
        return PB_ERR_NOTSUP;
    }
    const lock_req_t req = { .slot = slot, .unlock = unlock };
    if (xQueueSend(s_lock_q, &req, pdMS_TO_TICKS(20)) != pdTRUE) {
        return PB_ERR_BUSY; /* 上一动作未完成 */
    }
    return PB_OK;
}

static pb_err_t l_init(void *ctx, uint8_t slot)
{
    (void)ctx;
    return (slot < HX711_SLOT_NUM) ? PB_OK : PB_ERR_INVALID;
}

static pb_err_t l_lock(void *ctx, uint8_t slot)
{
    (void)ctx;
    return lock_act(slot, false);
}

static pb_err_t l_unlock(void *ctx, uint8_t slot)
{
    (void)ctx;
    return lock_act(slot, true);
}

static pb_err_t l_state(void *ctx, uint8_t slot, bool *locked)
{
    (void)ctx;
    if (slot >= HX711_SLOT_NUM || locked == NULL) {
        return PB_ERR_INVALID;
    }
    if (s_has_165) {
        const uint16_t v = sh165_read();
        *locked = (((v >> (8 + slot)) & 1) == LOCK_SENSE_LEVEL);
    } else {
        *locked = (s_unlocked_mask & (1u << slot)) == 0;
    }
    return PB_OK;
}

static const pb_lock_ops_t s_lock_ops = {
    .init = l_init, .lock = l_lock, .unlock = l_unlock, .get_state = l_state,
};

/* ---------------------------------------------------------------- 开盖 ops */

static pb_err_t lid_is_open(void *ctx, uint8_t slot, bool *open)
{
    (void)ctx;
    if (slot >= HX711_SLOT_NUM || open == NULL) {
        return PB_ERR_INVALID;
    }
    if (!s_has_165) {
        *open = false; /* 无到位开关：视为常闭，仅靠称重判定 */
        return PB_OK;
    }
    const uint16_t v = sh165_read();
    *open = (((v >> slot) & 1) == LID_OPEN_LEVEL);
    return PB_OK;
}

static const pb_lid_ops_t s_lid_ops = { .is_open = lid_is_open };

/* ---------------------------------------------------------------- 语音 ops */

static pb_err_t audio_speak(void *ctx, uint16_t phrase, const char *text)
{
    (void)ctx;
    (void)text; /* 药名不做离线 TTS，只按语编号播报 */

    const int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(PIN_AUDIO_BUSY) != 0) {
        if (esp_timer_get_time() - t0 > 5000000) {
            return PB_ERR_TIMEOUT; /* 模块卡住，不再等 */
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const uint8_t cmd[3] = { 0xFF, (uint8_t)(phrase >> 8), (uint8_t)(phrase & 0xFF) };
    const int n = uart_write_bytes(AUDIO_UART_NUM, cmd, sizeof(cmd));
    return (n == (int)sizeof(cmd)) ? PB_OK : PB_ERR_IO;
}

static pb_err_t audio_stop(void *ctx)
{
    (void)ctx;
    const uint8_t stop = 0xFE; /* WT588D 停止码 */
    uart_write_bytes(AUDIO_UART_NUM, &stop, 1);
    return PB_OK;
}

static const pb_audio_ops_t s_audio_ops = { .speak = audio_speak, .stop = audio_stop };

/* ------------------------------------------------------------------ 显示 */
/*
 * 本层只负责"状态可视化"：状态灯 + 整屏底色 + 背光。
 * 汉字文案渲染（ST7789 + LVGL + 中文字库）属于 ui/ 层扩展点，
 * 屏上要显示的文字统一由下面三个缓存字段带出，屏幕驱动可直接取用。
 */
typedef enum {
    UI_OFF = 0,
    UI_IDLE,
    UI_DUE,
    UI_MSG,
    UI_STORAGE,
} ui_mode_t;

static volatile ui_mode_t s_ui_mode;
static char s_ui_title[PB_TEXT_LEN];
static char s_ui_l1[PB_TEXT_LEN];
static char s_ui_l2[PB_TEXT_LEN];

static void led_set(bool r, bool g, bool b)
{
    gpio_set_level(PIN_LED_R, r);
    gpio_set_level(PIN_LED_G, g);
    gpio_set_level(PIN_LED_B, b);
}

static void lcd_fill(uint16_t color)
{
    static uint16_t s_last_color;
    static bool s_painted;

    if (s_panel == NULL || (s_painted && s_last_color == color)) {
        return; /* 同一底色不重复刷，避免 1Hz 调用时拖慢主循环 */
    }
    static uint16_t line[LCD_H_RES];
    for (int i = 0; i < LCD_H_RES; ++i) {
        line[i] = color;
    }
    for (int y = 0; y < LCD_V_RES; ++y) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + 1, line);
    }
    s_last_color = color;
    s_painted = true;
}

static void backlight_set(bool on)
{
    s_bl_duty = on ? 1023 : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, (uint32_t)s_bl_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static pb_err_t disp_due(void *ctx, uint8_t slot, const char *drug, uint8_t units, uint16_t stock)
{
    (void)ctx;
    snprintf(s_ui_title, sizeof(s_ui_title), "请取药 #%u", (unsigned)(slot + 1));
    snprintf(s_ui_l1, sizeof(s_ui_l1), "%s", drug != NULL ? drug : "");
    snprintf(s_ui_l2, sizeof(s_ui_l2), "每次 %u 片，余 %u", (unsigned)units, (unsigned)stock);
    s_ui_mode = UI_DUE;
    lcd_fill(0xFD20); /* 橙 */
    return PB_OK;
}

static pb_err_t disp_idle(void *ctx, int32_t seconds_to_next, int32_t next_slot)
{
    (void)ctx;
    if (next_slot < 0) {
        snprintf(s_ui_title, sizeof(s_ui_title), "暂无提醒");
        snprintf(s_ui_l1, sizeof(s_ui_l1), "");
    } else {
        snprintf(s_ui_title, sizeof(s_ui_title), "下一格 #%ld", (long)(next_slot + 1));
        snprintf(s_ui_l1, sizeof(s_ui_l1), "还有 %ld 分钟", (long)(seconds_to_next / 60));
    }
    snprintf(s_ui_l2, sizeof(s_ui_l2), "");
    s_ui_mode = UI_IDLE;
    lcd_fill(0x0320); /* 深绿 */
    return PB_OK;
}

static pb_err_t disp_message(void *ctx, const char *title, const char *line1, const char *line2)
{
    (void)ctx;
    snprintf(s_ui_title, sizeof(s_ui_title), "%s", title != NULL ? title : "");
    snprintf(s_ui_l1, sizeof(s_ui_l1), "%s", line1 != NULL ? line1 : "");
    snprintf(s_ui_l2, sizeof(s_ui_l2), "%s", line2 != NULL ? line2 : "");
    s_ui_mode = UI_MSG;
    lcd_fill(0x0210); /* 深蓝 */
    ESP_LOGI(TAG, "ui: %s / %s / %s", s_ui_title, s_ui_l1, s_ui_l2);
    return PB_OK;
}

static pb_err_t disp_storage(void *ctx)
{
    (void)ctx;
    snprintf(s_ui_title, sizeof(s_ui_title), "收纳模式");
    snprintf(s_ui_l1, sizeof(s_ui_l1), "电量过低，全部已解锁");
    snprintf(s_ui_l2, sizeof(s_ui_l2), "");
    s_ui_mode = UI_STORAGE;
    lcd_fill(0x001F); /* 蓝 */
    return PB_OK;
}

static pb_err_t disp_backlight(void *ctx, bool on)
{
    (void)ctx;
    backlight_set(on);
    return PB_OK;
}

static const pb_display_ops_t s_display_ops = {
    .show_due = disp_due, .show_idle = disp_idle, .show_message = disp_message,
    .show_storage = disp_storage, .backlight = disp_backlight,
};

static void ui_task(void *arg)
{
    (void)arg;
    bool blink = false;
    for (;;) {
        blink = !blink;
        switch (s_ui_mode) {
        case UI_IDLE:
            led_set(false, true, false);
            backlight_set(true);
            break;
        case UI_DUE:
            led_set(blink, blink, false);
            backlight_set(true);
            break;
        case UI_MSG:
            led_set(false, false, true);
            break;
        case UI_STORAGE:
            led_set(true, true, true);
            backlight_set(true);
            break;
        case UI_OFF:
        default:
            led_set(false, false, false);
            backlight_set(false);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ------------------------------------------------------------ I2C 与电量 */

static bool i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n)
{
    if (buf == NULL || n == 0) {
        return false;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)(addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)(addr << 1) | I2C_MASTER_READ, true);
    if (n > 1) {
        i2c_master_read(cmd, buf, n - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, buf + n - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    const esp_err_t err = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(60));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

static bool i2c_write_regs(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t n)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)(addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    if (n > 0 && buf != NULL) {
        i2c_master_write(cmd, buf, n, true);
    }
    i2c_master_stop(cmd);
    const esp_err_t err = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(60));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

static bool s_has_gauge;

static pb_err_t p_init(void *ctx)
{
    (void)ctx;
    uint8_t v[2] = { 0, 0 };
    s_has_gauge = i2c_read_reg(MAX17048_ADDR, 0x08, v, sizeof(v)); /* VERSION */
    if (!s_has_gauge) {
        ESP_LOGW(TAG, "MAX17048 not found, battery protection degraded");
    }
    return s_has_gauge ? PB_OK : PB_ERR_NOTSUP;
}

static pb_err_t p_read_mv(void *ctx, uint32_t *mv)
{
    (void)ctx;
    if (mv == NULL) {
        return PB_ERR_INVALID;
    }
    uint8_t b[2];
    if (!s_has_gauge || !i2c_read_reg(MAX17048_ADDR, 0x02, b, sizeof(b))) {
        return PB_ERR_NOTSUP;
    }
    /* VCELL: 78.125 µV/LSB -> mV = raw * 5 / 64 */
    const uint32_t raw = ((uint32_t)b[0] << 8) | b[1];
    *mv = (uint32_t)(((uint64_t)raw * 5u) / 64u);
    return PB_OK;
}

static pb_err_t p_read_soc(void *ctx, uint8_t *pct)
{
    (void)ctx;
    if (pct == NULL) {
        return PB_ERR_INVALID;
    }
    uint8_t b[2];
    if (!s_has_gauge || !i2c_read_reg(MAX17048_ADDR, 0x04, b, sizeof(b))) {
        return PB_ERR_NOTSUP;
    }
    /* SOC: 1/256 %，高字节即整数百分比 */
    *pct = b[0];
    return PB_OK;
}

static const pb_power_ops_t s_power_ops = {
    .init = p_init, .read_mv = p_read_mv, .read_soc = p_read_soc,
};

/* ---------------------------------------------------------------- RTC */

static bool    s_time_synced;
static int64_t s_anchor_unix;
static int64_t s_anchor_us;

static uint8_t bcd2bin(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

static uint8_t bin2bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* Howard Hinnant 的 days_from_civil，避免依赖 newlib 的时区实现 */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2) ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned dd = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned mm = mp + (mp < 10 ? 3u : (unsigned)-9);
    *y = (int)(yy + (mm <= 2 ? 1 : 0));
    *m = mm;
    *d = dd;
}

static void time_anchor(int64_t unix_s)
{
    s_anchor_unix = unix_s;
    s_anchor_us = esp_timer_get_time();
    s_time_synced = true;
}

bool hal_esp_set_rtc(int64_t unix_s)
{
    time_anchor(unix_s);
    if (unix_s <= 0) {
        return false;
    }

    const int64_t local = unix_s + s_tz_offset_s;
    int64_t days = local / 86400;
    int32_t rem = (int32_t)(local % 86400);
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    int y = 0;
    unsigned m = 0, d = 0;
    civil_from_days(days, &y, &m, &d);

    const uint8_t b[7] = {
        bin2bcd((uint8_t)(rem / 3600)),
        bin2bcd((uint8_t)((rem % 3600) / 60)),
        bin2bcd((uint8_t)(rem % 60)),
        bin2bcd((uint8_t)(1 + (days + 4) % 7)), /* 1=周日 */
        bin2bcd((uint8_t)d),
        bin2bcd((uint8_t)m),
        bin2bcd((uint8_t)(y % 100)),
    };
    const bool ok = i2c_write_regs(DS3231_ADDR, 0x00, b, sizeof(b));
    ESP_LOGI(TAG, "rtc set %04d-%02u-%02u %02u:%02u (%s)", y, m, d,
             (unsigned)(rem / 3600), (unsigned)((rem % 3600) / 60), ok ? "ok" : "fail");
    return ok;
}

bool hal_esp_resync_from_rtc(void)
{
    uint8_t b[7];
    if (!i2c_read_reg(DS3231_ADDR, 0x00, b, sizeof(b))) {
        return false;
    }
    const int y = 2000 + bcd2bin(b[6]);
    const unsigned m = bcd2bin(b[5]);
    const unsigned d = bcd2bin(b[4]);
    const int hh = bcd2bin(b[2]);
    const int mi = bcd2bin(b[1]);
    const int ss = bcd2bin((uint8_t)(b[0] & 0x7F));
    if (y < 2024 || m < 1 || m > 12 || d < 1 || d > 31 || hh > 23 || mi > 59 || ss > 59) {
        return false;
    }
    const int64_t local = days_from_civil(y, m, d) * 86400 + hh * 3600 + mi * 60 + ss;
    time_anchor(local - s_tz_offset_s);
    return true;
}

static pb_err_t t_unix(void *ctx, int64_t *sec)
{
    (void)ctx;
    if (sec == NULL) {
        return PB_ERR_INVALID;
    }
    if (!s_time_synced) {
        return PB_ERR_NOTSUP;
    }
    *sec = s_anchor_unix + (esp_timer_get_time() - s_anchor_us) / 1000000;
    return PB_OK;
}

static bool t_valid(void *ctx)
{
    (void)ctx;
    return s_time_synced;
}

static const pb_time_ops_t s_time_ops = { .unix_time = t_unix, .time_valid = t_valid };

void hal_esp_set_tz(int32_t offset_s)
{
    s_tz_offset_s = offset_s;
}

/* ---------------------------------------------------------------- 通知 ops */

static pb_err_t n_publish(void *ctx, const char *json)
{
    (void)ctx;
    if (json == NULL) {
        return PB_ERR_INVALID;
    }
    if (net_publish(json) == PB_OK) {
        return PB_OK;
    }
    oq_push(json); /* 断网：落 NVS，联网后由主循环补发 */
    return PB_ERR_IO;
}

static pb_err_t n_bracelet(void *ctx, uint8_t alert, uint8_t slot)
{
    (void)ctx;
    return ble_notify(alert, slot);
}

static const pb_notify_ops_t s_notify_ops = {
    .publish_event = n_publish, .notify_bracelet = n_bracelet,
};

/* ------------------------------------------------------------ 外设初始化 */

static void ledc_setup(void)
{
    const ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 4000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tcfg);

    ledc_channel_config_t ch = {
        .gpio_num   = PIN_LOCK_PWM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);

    ch.gpio_num = PIN_LCD_BL;
    ch.channel  = LEDC_CHANNEL_1;
    ledc_channel_config(&ch);
}

static void audio_uart_setup(void)
{
    const uart_config_t cfg = {
        .baud_rate  = 9600,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(AUDIO_UART_NUM, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "audio uart install failed");
        return;
    }
    uart_param_config(AUDIO_UART_NUM, &cfg);
    uart_set_pin(AUDIO_UART_NUM, PIN_AUDIO_TX, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static bool i2c_bus_init(void)
{
    const i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_I2C_SDA,
        .scl_io_num       = PIN_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_CLK_HZ,
    };
    if (i2c_param_config(I2C_PORT_NUM, &cfg) != ESP_OK) {
        return false;
    }
    const esp_err_t err = i2c_driver_install(I2C_PORT_NUM, I2C_MODE_MASTER, 0, 0, 0);
    return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}

/* ------------------------------------------------------------ 对上层接口 */

static const pb_hal_t s_hal = {
    .weight  = &s_weight_ops,
    .lock    = &s_lock_ops,
    .lid     = &s_lid_ops,
    .audio   = &s_audio_ops,
    .display = &s_display_ops,
    .power   = &s_power_ops,
    .notify  = &s_notify_ops,
    .time    = &s_time_ops,
};

esp_err_t hal_esp_init(void)
{
    gpio_out(PIN_SH595_DATA);
    gpio_out(PIN_SH595_CLK);
    gpio_out(PIN_SH595_LATCH);
    gpio_out(PIN_SH165_LOAD);
    gpio_out(PIN_SH165_CLK);
    gpio_out(PIN_HX711_SCK);
    gpio_out(PIN_LED_R);
    gpio_out(PIN_LED_G);
    gpio_out(PIN_LED_B);
    gpio_in(PIN_SH165_DATA, GPIO_PULLUP_ENABLE);
    gpio_in(PIN_AUDIO_BUSY, GPIO_PULLUP_ENABLE);

    led_set(false, false, false);
    gpio_set_level(PIN_SH165_LOAD, 1);
    gpio_set_level(PIN_SH165_CLK, 0);
    gpio_set_level(PIN_HX711_SCK, 0); /* HX711 时钟空闲为低，避免误触发采样 */
    sh595_write(0); /* 上电全部断电，靠蜗轮保持锁定 */

    ledc_setup();
    audio_uart_setup();
    if (!i2c_bus_init()) {
        ESP_LOGE(TAG, "i2c init failed, no RTC/gauge");
    }
    lcd_init();

    for (uint8_t i = 0; i < HX711_SLOT_NUM; ++i) {
        (void)w_init(NULL, i);
    }

    s_lock_q = xQueueCreate(4, sizeof(lock_req_t));
    if (s_lock_q != NULL) {
        xTaskCreate(lock_task, "pb_lock", 3072, NULL, 6, &s_lock_task);
    } else {
        ESP_LOGE(TAG, "lock queue alloc failed, locks disabled");
    }
    xTaskCreate(ui_task, "pb_ui", 3072, NULL, 2, NULL);

    (void)p_init(NULL);
    if (!hal_esp_resync_from_rtc()) {
        ESP_LOGW(TAG, "rtc invalid, wait for app/mqtt time sync");
    }
    ESP_LOGI(TAG, "hal ready (gauge=%d, rtc=%d, lcd=%d)",
             (int)s_has_gauge, (int)s_time_synced, (int)(s_panel != NULL));
    return ESP_OK;
}

const pb_hal_t *hal_esp_get(void)
{
    return &s_hal;
}

static void lcd_init(void)
{
    const spi_bus_config_t bus = {
        .sclk_io_num     = PIN_LCD_SCLK,
        .mosi_io_num     = PIN_LCD_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * 8 * (int)sizeof(uint16_t),
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGW(TAG, "lcd spi bus init failed");
        return;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = PIN_LCD_DC,
        .cs_gpio_num       = PIN_LCD_CS,
        .pclk_hz           = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 8,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io) != ESP_OK) {
        ESP_LOGW(TAG, "lcd panel io init failed");
        return;
    }

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = PIN_LCD_RST,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &dev, &s_panel) != ESP_OK) {
        ESP_LOGW(TAG, "st7789 init failed, UI falls back to LEDs");
        s_panel = NULL;
        return;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_disp_on_off(s_panel, true);
}



