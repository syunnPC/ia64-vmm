# SPDX-License-Identifier: GPL-2.0-or-later
CLANG_FORMAT ?= clang-format
RUFF ?= ruff
.PHONY: format check-format lint
format:
	python3 tools/format.py --clang-format '$(CLANG_FORMAT)'
	$(RUFF) format
check-format:
	python3 tools/format.py --check --clang-format '$(CLANG_FORMAT)'
	$(RUFF) format --check
lint:
	$(RUFF) check
