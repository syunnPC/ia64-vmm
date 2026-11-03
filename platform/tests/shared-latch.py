# SPDX-License-Identifier: GPL-2.0-or-later
"""Restore the former shared service selector in an isolated test build."""

from pathlib import Path

source = Path("platform/devices.c").read_text()
source = source.replace(
    "static unsigned service_port_width(",
    "static uint32_t shared_service_port;\nstatic unsigned service_port_width(",
    1,
)
source = source.replace("v->service_port", "shared_service_port")
source = source.replace(
    "{ unsigned p=shared_service_port", "{ (void)v; unsigned p=shared_service_port"
)
Path("build/tests/smp-shared-latch.c").write_text(source)
