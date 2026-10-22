#include "dose_verify.h"

#include <string.h>

#define PB_MIN_TOLERANCE_MG 60

void pb_evaluate_dose(const pb_slot_t *s, uint8_t units,
                      int32_t w_before_mg, int32_t w_after_mg,
                      uint32_t noise_mg, pb_dose_eval_t *out)
{
    pb_dose_eval_t r;
    memset(&r, 0, sizeof(r));

    if (s == NULL || !s->used || units == 0 || s->unit_mg == 0) {
        r.result = PB_DOSE_NONE;
        if (out != NULL) {
            *out = r;
        }
        return;
    }

    const int32_t expect = (int32_t)units * (int32_t)s->unit_mg;
    int32_t tol = expect * (int32_t)s->tolerance_pct / 100;
    if (tol < PB_MIN_TOLERANCE_MG) {
        tol = PB_MIN_TOLERANCE_MG;
    }

    const int32_t dw = w_before_mg - w_after_mg;

    /* 取药后格内剩余（相对空盒基准） */
    const int32_t remain = w_after_mg - s->empty_mg;
    const int32_t remain_expect =
        ((int32_t)s->stock - (int32_t)units > 0)
            ? ((int32_t)s->stock - (int32_t)units) * (int32_t)s->unit_mg
            : 0;
    const int32_t residual = remain - remain_expect;

    r.expect_mg   = expect;
    r.actual_mg   = dw;
    r.remain_mg   = remain;
    r.residual_mg = residual;

    if (dw < (int32_t)noise_mg) {
        /* 重量几乎没变化：完全没取（或开了盖又放回去） */
        r.result = PB_DOSE_NONE;
    } else if (dw > expect + tol) {
        r.result = PB_DOSE_OVER;
    } else if (dw < expect - tol) {
        r.result = PB_DOSE_PARTIAL;
    } else {
        /* 减重数量对得上，但要检查"取药后没有清零" */
        if (residual > tol + s->allow_residual_mg + (int32_t)noise_mg) {
            r.result = PB_DOSE_PARTIAL;
        } else {
            r.result = PB_DOSE_OK;
        }
    }

    if (out != NULL) {
        *out = r;
    }
}
