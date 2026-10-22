/*
 * dose_verify.h —— 取药量判定（纯函数，无副作用，便于穷举测试）
 */
#ifndef PB_DOSE_VERIFY_H
#define PB_DOSE_VERIFY_H

#include "pillbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 判定一次取药是否达标。
 *
 *  s           : 药格配置（只读；stock 为"取药前"的片数）
 *  units       : 本次应取片数
 *  w_before_mg : 开锁前净重
 *  w_after_mg  : 关盖稳定后净重
 *  noise_mg    : 称重噪声门限
 *  out         : 判定明细（可为 NULL）
 *
 * 判定规则：
 *   expect = units * unit_mg
 *   tol    = max(expect * tolerance_pct / 100, 60mg)
 *   dw     = w_before - w_after
 *   1) dw < noise                       → NONE
 *   2) dw > expect + tol                → OVER
 *   3) dw < expect - tol                → PARTIAL
 *   4) dw 在容差内但"残留超标"           → PARTIAL（这就是"取药后没有清零"）
 *      残留 = (w_after - empty_mg) - (stock - units) * unit_mg
 *      残留 > tol + allow_residual_mg + noise → PARTIAL
 *   5) 其余                             → OK
 */
void pb_evaluate_dose(const pb_slot_t *s, uint8_t units,
                      int32_t w_before_mg, int32_t w_after_mg,
                      uint32_t noise_mg, pb_dose_eval_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PB_DOSE_VERIFY_H */
