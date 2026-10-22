/*
 * stock.h —— 库存与药单校验（纯 C）
 */
#ifndef PB_STOCK_H
#define PB_STOCK_H

#include "pillbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 扣减库存，返回剩余片数（不足时归零） */
uint16_t pb_stock_consume(pb_slot_t *s, uint16_t units);

/* 补药，返回剩余片数 */
uint16_t pb_stock_add(pb_slot_t *s, uint16_t units);

/* 按每日总量估算还能吃几天 */
uint16_t pb_stock_days_left(const pb_slot_t *s);

/* 药单合法性校验（APP 下发时用） */
bool pb_slot_validate(const pb_slot_t *s);

#ifdef __cplusplus
}
#endif

#endif /* PB_STOCK_H */
