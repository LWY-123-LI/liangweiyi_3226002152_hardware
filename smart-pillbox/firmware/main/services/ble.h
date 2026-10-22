/*
 * ble.h —— 蓝牙手环直连通知（NimBLE 外设）
 * 手环连接后订阅 notify 特征，收到 1 字节 alert + 1 字节 slot。
 */
#ifndef PB_BLE_H
#define PB_BLE_H

#include "hal_if.h"

#include <stdbool.h>
#include <stdint.h>

void      ble_start(const char *device_name);
bool      ble_connected(void);
pb_err_t  ble_notify(uint8_t alert, uint8_t slot);

#endif /* PB_BLE_H */
