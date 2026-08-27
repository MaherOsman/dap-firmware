/* Minimal host-side test harness. No dependencies beyond libc. */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

static int t_fail_count;
static int t_check_count;
static const char *t_current;

#define TEST(name) static void name(void)

#define RUN(fn)                                                                \
    do {                                                                       \
        t_current = #fn;                                                       \
        int before = t_fail_count;                                             \
        fn();                                                                  \
        printf("  %-44s %s\n", #fn,                                            \
               (t_fail_count == before) ? "ok" : "FAIL");                      \
    } while (0)

#define CHECK(cond)                                                            \
    do {                                                                       \
        t_check_count++;                                                       \
        if (!(cond)) {                                                         \
            t_fail_count++;                                                    \
            printf("    %s:%d: CHECK failed in %s: %s\n", __FILE__, __LINE__,  \
                   t_current, #cond);                                          \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        t_check_count++;                                                       \
        long long _a = (long long)(a), _b = (long long)(b);                    \
        if (_a != _b) {                                                        \
            t_fail_count++;                                                    \
            printf("    %s:%d: %s == %s failed (%lld vs %lld)\n", __FILE__,    \
                   __LINE__, #a, #b, _a, _b);                                  \
        }                                                                      \
    } while (0)

#define TEST_SUMMARY()                                                         \
    (printf("  %d checks, %d failures\n", t_check_count, t_fail_count),        \
     t_fail_count == 0 ? 0 : 1)

#endif /* TEST_H */
