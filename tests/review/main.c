/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdbool.h>
#include <string.h>
#include "tests/review/review.h"

unsigned long long review_checks;
int review_mock_clock;
uint64_t review_mock_counter, review_mock_hz;

uint64_t __real_hal_counter(void);
uint64_t __real_hal_counter_hz(void);
uint64_t __wrap_hal_counter(void);
uint64_t __wrap_hal_counter_hz(void);
uint64_t __wrap_hal_counter(void)
{
    return review_mock_clock ? review_mock_counter : __real_hal_counter();
}
uint64_t __wrap_hal_counter_hz(void)
{
    return review_mock_clock ? review_mock_hz : __real_hal_counter_hz();
}

static struct {
    const char *name;
    void (*run)(void);
} suites[16];
static unsigned suite_count;
void review_register(const char *name, void (*run)(void))
{
    if (suite_count == sizeof(suites) / sizeof(suites[0])) {
        fprintf(stderr, "review: too many suites\n");
        exit(1);
    }
    suites[suite_count].name = name;
    suites[suite_count++].run = run;
}

/* No argument: every suite; otherwise only the named ones. */
int main(int argc, char **argv)
{
    for (unsigned i = 0; i < suite_count; ++i) {
        bool selected = argc < 2;
        for (int a = 1; a < argc; ++a)
            selected |= !strcmp(argv[a], suites[i].name);
        if (!selected)
            continue;
        unsigned long long before = review_checks;
        suites[i].run();
        printf("review: %s: %llu checks passed\n", suites[i].name, review_checks - before);
    }
    return 0;
}
