/* test_util.h —— 极简断言（无第三方依赖，PC 上直接 gcc 跑） */
#ifndef PB_TEST_UTIL_H
#define PB_TEST_UTIL_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern int g_checks;
extern int g_fails;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_fails++;                                                         \
            printf("    [FAIL] %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

#define CHECK_EQI(actual, expect)                                              \
    do {                                                                       \
        g_checks++;                                                            \
        const long _a = (long)(actual);                                        \
        const long _e = (long)(expect);                                        \
        if (_a != _e) {                                                        \
            g_fails++;                                                         \
            printf("    [FAIL] %s:%d  %s = %ld, 期望 %ld\n",                   \
                   __FILE__, __LINE__, #actual, _a, _e);                       \
        }                                                                      \
    } while (0)

#define CHECK_STREQ(actual, expect)                                            \
    do {                                                                       \
        g_checks++;                                                            \
        const char *_a = (actual);                                             \
        const char *_e = (expect);                                             \
        if (_a == NULL || _e == NULL || strcmp(_a, _e) != 0) {                 \
            g_fails++;                                                         \
            printf("    [FAIL] %s:%d  %s = \"%s\", 期望 \"%s\"\n",            \
                   __FILE__, __LINE__, #actual, _a ? _a : "(null)",             \
                   _e ? _e : "(null)");                                        \
        }                                                                      \
    } while (0)

#define RUN_TEST(fn)                                                           \
    do {                                                                       \
        const int _before = g_fails;                                           \
        printf("-- %s\n", #fn);                                                \
        fn();                                                                  \
        printf("   %s\n", (g_fails == _before) ? "PASS" : "FAILED");           \
    } while (0)

/* 各测试文件入口 */
void test_dose_verify(void);
void test_schedule_and_stock(void);
void test_session(void);
void test_safety(void);

#endif /* PB_TEST_UTIL_H */
