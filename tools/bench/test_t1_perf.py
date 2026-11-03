# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic perf records: classification must preserve sampling limits."""

import collections
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import t1_perf as perf  # noqa: E402


class ProductProfileTest(unittest.TestCase):
    def classify(self, frames, hosted=None):
        arena = {"t1": (0x1000, 0x9000), "t0": (0xA000, 0xB000)}
        detail = collections.defaultdict(collections.Counter)
        counts, total = perf.product_categorize(
            "", [0x10000], arena, [], set(), detail, [frames], hosted
        )
        self.assertEqual(total, 1)
        name = next(iter(counts))
        return next(group for category, group, _ in perf.PRODUCT_CATS if category == name)

    def test_leaf_runtime_bookkeeping(self):
        for sym in ("publish_running", "prepare_entry", "finish_execute", "native_handoff"):
            with self.subTest(sym=sym):
                self.assertEqual(self.classify([[0x10000, sym, "dispatch.c", 1]]), perf.OVERHEAD)

    def test_hosted_current_context_is_separate(self):
        self.assertEqual(
            self.classify([[0x10000, "t1_set_current", "region.h", 10]], {"region.h": {10}}),
            perf.HOSTED,
        )

    def test_hosted_materialize_checks_are_separate(self):
        frames = [
            [0x10000, "t1_home_read_at", "materialize.c", 20],
            [0x10001, "t1_materialize", "materialize.c", 100],
        ]
        self.assertEqual(self.classify(frames, {"materialize.c": {100}}), perf.HOSTED)
        self.assertEqual(self.classify(frames[:1]), perf.MATERIALIZE)

    def test_validity_leaf_is_shared_but_caller_disambiguates(self):
        frames = [[0x10000, "enter_mask", "validity.c", 20]]
        self.assertEqual(self.classify(frames), perf.SHARED)
        self.assertEqual(self.classify(frames + [[0x10001, "enter", "tier0.c", 200]]), perf.TIER0)
        self.assertEqual(
            self.classify(frames + [[0x10001, "validate_entry", "dispatch.c", 400]]), perf.OVERHEAD
        )

    def test_symbol_suffix_is_ignored(self):
        self.assertEqual(
            self.classify([[0x10000, "publish_running.constprop.0+0x12", "dispatch.c", 1]]),
            perf.OVERHEAD,
        )

    def test_hook_condition_can_grow(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "tier0.c"
            source.write_text(
                "bool skipped = skip;\nskip = false;\n"
                "if (upper && !skipped && !(v->s.psr & upper->lower_psr)) {\n"
                "    upper->enter();\n}\nother();\n"
            )
            self.assertEqual(perf.tier0_hook_lines(source), set(range(1, 6)))

    def test_missing_hook_has_useful_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "tier0.c"
            source.write_text("void run(void) {}\n")
            with self.assertRaisesRegex(SystemExit, "upper-tier hook"):
                perf.tier0_hook_lines(source)

    def test_hosted_line_map_tracks_else_and_nested_conditions(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "region.h"
            source.write_text(
                "#ifdef IA64VMM_HOSTED\ncheck();\n#if FLAG\ninner();\n"
                "#endif\n#else\nproduction();\n#endif\nshared();\n"
            )
            self.assertEqual(perf.hosted_source_lines([source])["region.h"], {2, 4})

    def test_region_metadata_uses_keys(self):
        arena, regions = perf.parse_bench(
            "jobdump-arena tier1 0x1000 0x9000 tier0 0xa000 0xb000\n"
            "jobdump-region 1 native 32 code 0x2000 size 512 loop 64 256 entries 10\n"
            "jobrun warm entries 10\n"
        )
        self.assertEqual(arena["entries"], 10)
        self.assertEqual(regions, [(0x2000, 512, 64, 256)])

    def test_generated_restore_span_precedes_loop_partition(self):
        arena, regions = perf.parse_bench(
            "bench-arena tier1 0x1000 0x9000 tier0 0xa000 0xb000 entries 10\n"
            "bench-region 1 code 0x2000 size 512 loop 64 256 entries 10 restore 400 512\n"
        )
        self.assertEqual(perf.jit_key(0x2000 + 450, arena, regions), "@jit-restore")
        self.assertEqual(perf.jit_key(0x2000 + 399, arena, regions), "@jit-exit")
        self.assertEqual(perf.jit_key(0x2200, arena, regions), "@jit-gone")

    def test_generated_frame_in_and_restore_have_separate_spans(self):
        arena, regions = perf.parse_bench(
            "bench-arena tier1 0x1000 0x9000 tier0 0xa000 0xb000 entries 10\n"
            "bench-region 1 code 0x2000 size 512 loop 64 256 entries 10 "
            "restore 400 512 frame-in 350 400\n"
        )
        self.assertEqual(perf.jit_key(0x2000 + 349, arena, regions), "@jit-exit")
        self.assertEqual(perf.jit_key(0x2000 + 350, arena, regions), "@jit-frame-in")
        self.assertEqual(perf.jit_key(0x2000 + 400, arena, regions), "@jit-restore")


if __name__ == "__main__":
    unittest.main()
