/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "test.h"

int main(void)
{
    test_format();
    test_log();
    test_guest_serial();
    test_log_matching();
    test_guest_mem();
    test_hal();
    test_options();
    test_dump_va();
    test_string();
    test_host_wake();
    puts(
        "test-host: formatter, log ring, guest memory, Linux HAL, options merge, string functions and the host wake protocol passed");
    return 0;
}
