/* main.c —— 测试入口（PC 上直接跑，不依赖任何框架） */
#include "test_util.h"

#include <stdio.h>

int g_checks = 0;
int g_fails = 0;

int main(void)
{
    printf("=== 智能药盒 core 单元测试 (host) ===\n\n");

    RUN_TEST(test_dose_verify);
    RUN_TEST(test_schedule_and_stock);
    RUN_TEST(test_session);
    RUN_TEST(test_safety);

    printf("\n----------------------------------------\n");
    printf("checks: %d, failed: %d\n", g_checks, g_fails);
    printf("%s\n", (g_fails == 0) ? "RESULT: ALL PASSED" : "RESULT: FAILED");
    return (g_fails == 0) ? 0 : 1;
}
