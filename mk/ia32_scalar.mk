# SPDX-License-Identifier: GPL-2.0-or-later
TEST_TARGETS += test-ia32-scalar
.PHONY: test-ia32-scalar
test-ia32-scalar: build/tests/ia32-scalar
	./build/tests/ia32-scalar
build/tests/ia32-scalar: tests/host/ia32/scalar.c ia64/ia32/scalar.c ia64/ia32/scalar.h ia64/ia32/decode.c ia64/ia32/decode.h ia64/ia32/access.h include/ia64vmm/ia32.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude -I. $(filter %.c,$^) -o $@
