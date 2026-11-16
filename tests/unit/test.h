/* Tiny test harness: TEST(name) { CHECK(...); } and RUN(name) from main(). */
#ifndef PAL_TEST_H
#define PAL_TEST_H

#include <stdio.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        g_checks++;                                                                                          \
        if (!(cond)) {                                                                                       \
            g_failures++;                                                                                    \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                        \
        }                                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                                       \
    do {                                                                                                     \
        long long _a = (long long)(a), _b = (long long)(b);                                                  \
        g_checks++;                                                                                          \
        if (_a != _b) {                                                                                      \
            g_failures++;                                                                                    \
            printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b);          \
        }                                                                                                    \
    } while (0)

#define TEST(name) static void name(void)
#define RUN(name)                                                                                            \
    do {                                                                                                     \
        int _before = g_failures;                                                                            \
        name();                                                                                              \
        printf("%s %s\n", g_failures == _before ? "ok  " : "FAIL", #name), fflush(stdout);                    \
    } while (0)
#define TEST_DONE()                                                                                          \
    (printf("%d checks, %d failures\n", g_checks, g_failures), g_failures ? 1 : 0)

#endif
