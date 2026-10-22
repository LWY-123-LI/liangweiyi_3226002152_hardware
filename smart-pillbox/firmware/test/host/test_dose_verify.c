/* test_dose_verify.c —— 取药判定规则（纯函数，穷举边界） */
#include "test_util.h"

#include "dose_verify.h"

#include <string.h>

/* 默认药格：单片 200mg，格内 10 片，空盒 1000mg → 满盒 3000mg */
static pb_slot_t base_slot(void)
{
    pb_slot_t s;
    memset(&s, 0, sizeof(s));
    s.used = true;
    snprintf(s.name, sizeof(s.name), "%s", "阿司匹林");
    s.unit_mg = 200;
    s.stock = 10;
    s.empty_mg = 1000;
    s.tolerance_pct = 15;
    s.take_window_s = 900;
    s.dose_n = 1;
    s.dose[0].minute = 480;
    s.dose[0].units = 1;
    return s;
}

void test_dose_verify(void)
{
    pb_slot_t s;
    pb_dose_eval_t ev;

    /* 1) 正常取 1 片：3000 → 2800 */
    s = base_slot();
    pb_evaluate_dose(&s, 1, 3000, 2800, 150, &ev);
    CHECK(ev.result == PB_DOSE_OK);
    CHECK_EQI(ev.expect_mg, 200);
    CHECK_EQI(ev.actual_mg, 200);
    CHECK_EQI(ev.remain_mg, 1800);
    CHECK_EQI(ev.residual_mg, 0);

    /* 2) 完全没取：dw = 0 < noise */
    s = base_slot();
    pb_evaluate_dose(&s, 1, 3000, 3000, 150, &ev);
    CHECK(ev.result == PB_DOSE_NONE);

    /* 3) 只有噪声：dw = 90 < 150 仍判 NONE */
    s = base_slot();
    pb_evaluate_dose(&s, 1, 3000, 2910, 150, &ev);
    CHECK(ev.result == PB_DOSE_NONE);

    /* 3b) 半片（100mg）低于噪声门限 → 只能判 NONE。
     *     这也是硬件选型要求：噪声必须远小于"最小可分辨剂量"，
     *     否则 ACTION 里要把 noise_mg 调到 60 以下并保证机械回零重复性。 */
    s = base_slot();
    pb_evaluate_dose(&s, 1, 3000, 2900, 150, &ev);
    CHECK(ev.result == PB_DOSE_NONE);

    /* 4) 少取：该取 2 片只取 1 片（dw=200 > 噪声，但 < 400-60） */
    s = base_slot();
    pb_evaluate_dose(&s, 2, 3000, 2800, 150, &ev);
    CHECK(ev.result == PB_DOSE_PARTIAL);
    CHECK_EQI(ev.actual_mg, 200);

    /* 5) 多取：dw = 300 > 200+60 */
    s = base_slot();
    pb_evaluate_dose(&s, 1, 3000, 2700, 150, &ev);
    CHECK(ev.result == PB_DOSE_OVER);
    CHECK_EQI(ev.actual_mg, 300);

    /* 6) 容差上边界：dw = 260 恰好等于 expect+tol → 不算多取 */
    s = base_slot();
    pb_evaluate_dose(&s, 1, 3000, 2740, 150, &ev);
    CHECK(ev.result == PB_DOSE_OK);
    CHECK_EQI(ev.residual_mg, -60);

    /* 7) 一次取 2 片：expect=400，仍是 OK */
    s = base_slot();
    pb_evaluate_dose(&s, 2, 3000, 2600, 150, &ev);
    CHECK(ev.result == PB_DOSE_OK);
    CHECK_EQI(ev.expect_mg, 400);
    CHECK_EQI(ev.remain_mg, 1600);

    /* 8) 减重对得上但"没清零"：空盒基准偏 200mg → 残留 200 > 60+0+50 */
    s = base_slot();
    s.empty_mg = 800;
    pb_evaluate_dose(&s, 1, 3000, 2800, 50, &ev);
    CHECK_EQI(ev.actual_mg, 200);
    CHECK_EQI(ev.residual_mg, 200);
    CHECK(ev.result == PB_DOSE_PARTIAL);

    /* 9) 同上，但药单声明允许残留（掰半片）→ 判 OK */
    s = base_slot();
    s.empty_mg = 800;
    s.allow_residual_mg = 200;
    pb_evaluate_dose(&s, 1, 3000, 2800, 50, &ev);
    CHECK(ev.result == PB_DOSE_OK);

    /* 10) 容差百分比生效：400mg 片，25% → tol=100，差 90 可取 */
    s = base_slot();
    s.unit_mg = 400;
    s.tolerance_pct = 25;
    pb_evaluate_dose(&s, 1, 5000, 4690, 150, &ev);
    CHECK(ev.result == PB_DOSE_OK);
    pb_evaluate_dose(&s, 1, 5000, 4710, 150, &ev); /* 差 110 > 100 */
    CHECK(ev.result == PB_DOSE_PARTIAL);

    /* 11) 最后一格：stock == units，取完后应为 0 */
    s = base_slot();
    s.stock = 1;
    pb_evaluate_dose(&s, 1, 1200, 1000, 150, &ev);
    CHECK(ev.result == PB_DOSE_OK);
    CHECK_EQI(ev.remain_mg, 0);
    CHECK_EQI(ev.residual_mg, 0);

    /* 12) 参数非法：units=0 视为没取 */
    s = base_slot();
    pb_evaluate_dose(&s, 0, 3000, 2800, 150, &ev);
    CHECK(ev.result == PB_DOSE_NONE);

    /* 13) 结果名可读性（上报/日志用） */
    CHECK_STREQ(pb_dose_result_name(PB_DOSE_OK), "ok");
    CHECK_STREQ(pb_dose_result_name(PB_DOSE_PARTIAL), "partial");
    CHECK_STREQ(pb_dose_result_name(PB_DOSE_NONE), "none");
    CHECK_STREQ(pb_dose_result_name(PB_DOSE_OVER), "over");
}
