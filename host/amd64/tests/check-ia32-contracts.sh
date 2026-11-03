#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run from the repository root; artifacts stay in build/, no machine paths.
set -eu
cc=${HOSTCC:-gcc}
# The product's ISA baseline (mk/config.mk BM_SIMD_CFLAGS).
simd=${BM_SIMD_CFLAGS:?BM_SIMD_CFLAGS is set by make}
out=build/ia32-contracts
mkdir -p "$out"
# Discard unused engine functions so this test brackets only the new weak
# defaults; log/dump sinks are counted in the fixture, no engine is run.
$cc -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
    $simd -ffunction-sections -fdata-sections -Iinclude -I. \
    engine/engine.c core/string.c host/amd64/tests/ia32_contracts.c \
    -Wl,--gc-sections -o "$out/defaults"
"$out/defaults"
for tier in 0 1; do
    define=
    if [ "$tier" = 1 ]; then define=-DIA32_TEST_TIER1=1; fi
    $cc -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
        $simd -ffunction-sections -fdata-sections -Iinclude -I. \
        -DIA64VMM_HOSTED=1 $define host/amd64/tests/ia32_clones.c core/string.c \
        -Wl,--gc-sections -o "$out/clones-tier$tier"
    "$out/clones-tier$tier"
done
printf '%s\n' 'ia32 contracts: unavailable defaults and both checker clones passed'
