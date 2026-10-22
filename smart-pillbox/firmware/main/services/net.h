/*
 * net.h —— Wi-Fi + MQTT 上行通道
 * 断网时事件进 offline_queue，恢复后按序补发。
 */
#ifndef PB_NET_H
#define PB_NET_H

#include "hal_if.h"

#include <stdbool.h>
#include <stdint.h>

#define PB_NET_TOPIC_LEN 96

typedef struct {
    char ssid[32];
    char pass[64];
    char uri[96];       /* mqtt://host:1883 或 mqtts://... */
    char device_id[24]; /* 设备唯一标识，用于 topic 与 payload */
} net_cfg_t;

/* 下行命令回调：json 形如 {"cmd":"unlock","slot":1}
 * 注意：core 不是线程安全的，命令一律进队列，由主循环统一取出执行。 */
typedef void (*net_cmd_cb_t)(const char *json, void *ctx);

void      net_start(const net_cfg_t *cfg);
bool      net_connected(void);
pb_err_t  net_publish(const char *json);
/* 取出下一条下行命令，返回 false 表示队列空 */
bool      net_take_command(char *out, size_t n);
const char *net_device_id(void);

#endif /* PB_NET_H */
