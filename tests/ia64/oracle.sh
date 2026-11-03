#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
exec python3 tests/ia64/oracle.py "${QEMU_IA64:?QEMU_IA64 is not set (see local.env.example)}" "$@"
