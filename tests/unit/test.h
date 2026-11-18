/* Tiny test harness: TEST(name) { CHECK(...); } and RUN(name) from main(). */
#ifndef PAL_TEST_H
#define PAL_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
/* PAL_TEST_TIME=1 prints how long each test took (finding the slow ones). */
static double test_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#define RUN(name)                                                                                            \
    do {                                                                                                     \
        int _before = g_failures;                                                                            \
        double _t0 = test_now_ms();                                                                          \
        name();                                                                                              \
        if (getenv("PAL_TEST_TIME")) printf("%s %s (%.0f ms)\n", g_failures == _before ? "ok  " : "FAIL", #name, test_now_ms() - _t0); \
        else printf("%s %s\n", g_failures == _before ? "ok  " : "FAIL", #name);                               \
        fflush(stdout);                                                                                      \
    } while (0)
#define TEST_DONE()                                                                                          \
    (printf("%d checks, %d failures\n", g_checks, g_failures), g_failures ? 1 : 0)

#endif
