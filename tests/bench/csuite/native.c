/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The compiled-C suite on the host (x86-64), for checksums independent of
 * every IA-64 engine. Usage: csuite-native KERNEL ITERS */
#include <stdio.h>
#include <stdlib.h>
#include "csuite.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: csuite-native KERNEL ITERS\n");
        return 2;
    }
    unsigned long long k = strtoull(argv[1], NULL, 0), n = strtoull(argv[2], NULL, 0);
    printf("csuite-native %llu %llu %#llx\n", k, n, (unsigned long long)csuite_main(k, n));
    return 0;
}
