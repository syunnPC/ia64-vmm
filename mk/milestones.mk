# SPDX-License-Identifier: GPL-2.0-or-later
# Executor milestone lines. The QEMU runners
# (tools/qemu/check-log.py, tests/qemu/apps.py) check the lines of their own
# runs with tests/hosted/milestones.py; this
# target checks that the checker rejects broken logs, and the same for the
# tier1 and hostwin statistics lines (tests/hosted/tier1stats.py). Part of
# `make test`.
TEST_TARGETS += test-milestones
.PHONY: test-milestones
test-milestones:
	@mkdir -p build/tests
	@python3 -I tests/hosted/milestones.py --self-test > build/tests/milestones-self-test.log 2>&1 || \
		{ cat build/tests/milestones-self-test.log >&2; exit 1; }
	@tail -1 build/tests/milestones-self-test.log
	@python3 -I tests/hosted/tier1stats.py --self-test > build/tests/tier1stats-self-test.log 2>&1 || \
		{ cat build/tests/tier1stats-self-test.log >&2; exit 1; }
	@tail -1 build/tests/tier1stats-self-test.log
