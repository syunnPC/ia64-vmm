#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
nm=$1
shift
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"$nm" -g --defined-only "$@" | awk 'NF == 3 {print $3}' > "$tmp/defined"
"$nm" -u "$@" | awk 'NF == 2 {print $2}' | sort -u > "$tmp/undefined"
# Compare the union, rather than each object in isolation: core objects call
# each other, but an accidental libc/runtime dependency must still fail.
awk 'FILENAME == ARGV[1] {defined[$1] = 1; next}
     !defined[$1] && $1 !~ /^hal_/ && $1 !~ /^(memcpy|memset|memmove|memcmp|strlen|strcmp|strncmp)$/ {
         print "unexpected freestanding dependency: " $1; bad = 1
     }
     END {exit bad}' "$tmp/defined" "$tmp/undefined"
echo 'check-freestanding: passed'
