#!/usr/bin/env python3
"""
Secondary regression: research compressor total still beats stock Brotli q=11
on the fixed corpus (historical goal). Primary gate is test_beat_zstd22.py.

Calls the real shipped compressors — no re-implementation of size logic.
"""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import brotli  # noqa: E402
from bav.codec import compress as bav_compress  # noqa: E402
from bav.codec import decompress as bav_decompress  # noqa: E402


class TestBeatBrotli(unittest.TestCase):
    def test_total_beats_google_brotli(self):
        cfg = json.loads((ROOT / "benchmarks" / "config.json").read_text(encoding="utf-8"))
        # Prefer secondary_baseline; fall back if older config shape
        sec = cfg.get("secondary_baseline") or {}
        quality = int(sec.get("quality", 11))
        method = cfg["research"]["method"]
        corpus = ROOT / cfg["corpus_dir"]

        br_total = 0
        bav_total = 0
        for name in cfg["corpus_files"]:
            path = corpus / name
            self.assertTrue(path.is_file(), f"missing {path}")
            raw = path.read_bytes()
            br = brotli.compress(raw, quality=quality)
            bav = bav_compress(raw, method=method)
            self.assertEqual(brotli.decompress(br), raw)
            self.assertEqual(bav_decompress(bav), raw)
            br_total += len(br)
            bav_total += len(bav)

        self.assertLess(
            bav_total,
            br_total,
            f"BAV total {bav_total} did not beat Brotli total {br_total}",
        )


if __name__ == "__main__":
    unittest.main()
