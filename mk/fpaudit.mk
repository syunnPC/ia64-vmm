# SPDX-License-Identifier: GPL-2.0-or-later
# Independent FP differential audit (tests/fuzz/fp). `make fpaudit` builds the
# hosted harness, compares it against the exact Python reference model
# (fpaudit.py; divergence groups are dumped to build/fpaudit-cases/), and runs
# the SDM Vol.1 12.3.3 divide/sqrt sequences end to end (fpseq.py).
#   make fpaudit FPAUDIT_ARGS='--count 200000'      # cases per family
#   make fpaudit FPAUDIT_ARGS='--knob u_trap_sets_i' # replay a historical model hypothesis
# Curated minimized cases: build/fpaudit-harness < tests/fuzz/fp/cases/<file>
# This file is read before mk/ia64.mk, so the source list is spelled out here.
FPAUDIT_SOURCES = tests/fuzz/fp/fp_harness.c \
	$(wildcard ia64/*.c ia64/decode/*.c ia64/ia32/*.c engine/interp/*.c engine/cache/*.c engine/tier0/*.c) engine/engine.c \
	core/log.c core/guest_mem.c tests/hal/hal_linux.c
FPAUDIT_OBJECTS = $(addprefix build/hosted/,$(FPAUDIT_SOURCES:.c=.o))
FPAUDIT_ARGS ?=
FPSEQ_ARGS ?= --count 20000

.PHONY: fpaudit
fpaudit: build/fpaudit-harness
	python3 tests/fuzz/fp/fpseq.py --harness build/fpaudit-harness $(FPSEQ_ARGS)
	python3 tests/fuzz/fp/fpseq.py --harness build/fpaudit-harness --wide $(FPSEQ_ARGS)
	python3 tests/fuzz/fp/fpaudit.py --harness build/fpaudit-harness $(FPAUDIT_ARGS)
build/fpaudit-harness: $(FPAUDIT_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(FPAUDIT_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include build/hosted/tests/fuzz/fp/fp_harness.d
