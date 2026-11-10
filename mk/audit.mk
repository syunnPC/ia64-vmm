# SPDX-License-Identifier: GPL-2.0-or-later
AUDIT_SOURCES = tests/audit/repro/tear.c $(wildcard ia64/*.c ia64/decode/*.c ia64/ia32/*.c engine/interp/*.c engine/cache/*.c engine/tier0/*.c) engine/engine.c core/log.c core/guest_mem.c tests/hal/hal_linux.c
AUDIT_OBJECTS = $(addprefix build/hosted/,$(AUDIT_SOURCES:.c=.o))
.PHONY: test-audit
test-ia64:: test-audit
test-audit: build/audit-tear
	./build/audit-tear
build/audit-tear: $(AUDIT_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(AUDIT_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -pthread -o $@
-include $(AUDIT_OBJECTS:.o=.d)
