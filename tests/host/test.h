/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TEST_HOST_H
#define TEST_HOST_H
#include <stdio.h>
#include <stdlib.h>
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                 \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
void test_format(void);
void test_log(void);
void test_log_matching(void);
void test_guest_serial(void);
void test_guest_mem(void);
void test_hal(void);
void test_options(void);
void test_dump_va(void);
void test_string(void);
void test_host_wake(void);
#endif
