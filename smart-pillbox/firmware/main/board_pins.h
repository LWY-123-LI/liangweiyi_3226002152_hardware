/*
 * board_pins.h —— 引脚分配（ESP32-S3-WROOM-1，8MB Flash + 8MB Octal PSRAM）
 *
 * ！！注意！！
 *   - GPIO26~GPIO37 被 Flash / Octal PSRAM 占用，本表全部避开；
 *   - GPIO0 为 BOOT 按键（复用为"确认"），GPIO45/46 为上电 strapping，
 *     只做输入（按键）用途；GPIO19/20 为 USB，未使用；
 *   - 打样前请按最终原理图复核一次。
 */
#ifndef PB_BOARD_PINS_H
#define PB_BOARD_PINS_H

#include "driver/gpio.h"

/* ---------------- I2C0：MAX17048 电量计 + DS3231 RTC ---------------- */
#define PIN_I2C_SDA          1
#define PIN_I2C_SCL          2
#define I2C_PORT_NUM         0
#define I2C_CLK_HZ           100000
#define MAX17048_ADDR        0x36
#define DS3231_ADDR          0x68

/* ---------------- HX711 × 8：共用 SCK，各自 DOUT ---------------- */
#define PIN_HX711_SCK        4
#define HX711_SLOT_NUM       8
/* 在 hal_esp.c 里组成数组使用，避免头文件里出现 static 数组 */
#define PIN_HX711_DOUT_0     GPIO_NUM_5
#define PIN_HX711_DOUT_1     GPIO_NUM_6
#define PIN_HX711_DOUT_2     GPIO_NUM_7
#define PIN_HX711_DOUT_3     GPIO_NUM_15
#define PIN_HX711_DOUT_4     GPIO_NUM_16
#define PIN_HX711_DOUT_5     GPIO_NUM_17
#define PIN_HX711_DOUT_6     GPIO_NUM_18
#define PIN_HX711_DOUT_7     GPIO_NUM_8

/* ---------------- 锁驱动：2 × 74HC595 串成 16 位（每格 IN1/IN2） ---------------- */
#define PIN_SH595_DATA       9
#define PIN_SH595_CLK        10
#define PIN_SH595_LATCH      11
#define PIN_LOCK_PWM         12   /* 电机总使能，LEDC 通道 0 */

/* ---------------- 74HC165 × 2：16 路并行输入（8 路开盖 + 8 路锁到位） ---------------- */
#define PIN_SH165_DATA       13
#define PIN_SH165_CLK        14
#define PIN_SH165_LOAD       21

/* ---------------- 语音：WT588D 一线串口 ---------------- */
#define PIN_AUDIO_TX         43
#define PIN_AUDIO_BUSY       44
#define AUDIO_UART_NUM       1

/* ---------------- TFT：ST7789 240×240（SPI2） ---------------- */
#define PIN_LCD_SCLK         47
#define PIN_LCD_MOSI         48
#define PIN_LCD_CS           38
#define PIN_LCD_DC           39
#define PIN_LCD_RST          40
#define PIN_LCD_BL           41
#define LCD_H_RES            240
#define LCD_V_RES            240
#define LCD_PIXEL_CLK_HZ     (20 * 1000 * 1000)

/* ---------------- 按键（低电平有效，内部上拉） ---------------- */
#define PIN_KEY_OK           0
#define PIN_KEY_SILENCE      45

/* ---------------- 状态灯（RGB） ---------------- */
#define PIN_LED_R            3
#define PIN_LED_G            46
#define PIN_LED_B            42

/* 电机动作脉宽：足够走完全行程，再断电由蜗轮蜗杆自锁 */
#define LOCK_PULSE_MS        450

#endif /* PB_BOARD_PINS_H */
