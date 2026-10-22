/*
 * offline_queue.h —— 断网事件缓存（NVS 环形队列，先进先出）
 * 事件为 core 产出的 JSON 原文，恢复网络后按序补发再删除。
 */
#ifndef PB_OFFLINE_QUEUE_H
#define PB_OFFLINE_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PB_OQ_CAPACITY 8
#define PB_OQ_ITEM_LEN 312

void     oq_init(void);
bool     oq_push(const char *json);            /* 满时丢弃最旧 */
bool     oq_peek(char *out, size_t n);         /* 只看不删 */
void     oq_drop(void);                        /* 删除队首（发送成功后调用） */
uint16_t oq_count(void);
void     oq_clear(void);

#endif /* PB_OFFLINE_QUEUE_H */
