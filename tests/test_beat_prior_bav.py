#!/usr/bin/env python3
"""
Gating test: shipped compress must strictly beat the frozen prior-BAV baseline
on every fixed corpus file and on the total.

Uses live bav.compress lengths vs constants in benchmarks/prior_bav_baseline.json.
Does not hardcode expected *new* sizes.
"""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from bav.codec import compress as bav_compress  # noqa: E402
from bav.codec import decompress as bav_decompress  # noqa: E402


class TestBeatPriorBav(unittest.TestCase):
    def test_every_file_and_total_beats_frozen_prior_bav(self):
        cfg = json.loads((ROOT / "benchmarks" / "config.json").read_text(encoding="utf-8"))
        prior = json.loads(
            (ROOT / "benchmarks" / "prior_bav_baseline.json").read_text(encoding="utf-8")
        )
        method = cfg["research"]["method"]
        corpus = ROOT / cfg["corpus_dir"]

        frozen_total = 0
        new_total = 0
        for name in cfg["corpus_files"]:
            with self.subTest(file=name):
                path = corpus / name
                self.assertTrue(path.is_file(), f"missing {path}")
                self.assertIn(name, prior["files"])
                frozen = int(prior["files"][name])
                raw = path.read_bytes()
                frame = bav_compress(raw, method=method)
                self.assertEqual(bav_decompress(frame), raw, f"round-trip failed: {name}")
                self.assertLess(
                    len(frame),
                    frozen,
                    f"{name}: new {len(frame)} did not beat frozen prior-BAV {frozen}",
                )
                frozen_total += frozen
                new_total += len(frame)

        self.assertEqual(frozen_total, int(prior["total"]))
        self.assertLess(
            new_total,
            frozen_total,
            f"new total {new_total} did not beat frozen total {frozen_total}",
        )


if __name__ == "__main__":
    unittest.main()
