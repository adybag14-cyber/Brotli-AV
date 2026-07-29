#!/usr/bin/env python3
"""
Secondary regression: research compressor must beat pure Zstd level 22 on every
fixed corpus file and on the total (prior goal).
"""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import zstandard as zstd  # noqa: E402
from bav.codec import compress as bav_compress  # noqa: E402
from bav.codec import decompress as bav_decompress  # noqa: E402


def _pure_zstd22(data: bytes, level: int = 22) -> bytes:
    return zstd.ZstdCompressor(level=level).compress(data)


class TestBeatZstd22(unittest.TestCase):
    def test_every_file_and_total_beats_zstd22(self):
        cfg = json.loads((ROOT / "benchmarks" / "config.json").read_text(encoding="utf-8"))
        zcfg = cfg.get("secondary_baseline_zstd") or cfg.get("baseline") or {}
        level = int(zcfg.get("level", 22))
        method = cfg["research"]["method"]
        corpus = ROOT / cfg["corpus_dir"]

        z_total = 0
        bav_total = 0
        for name in cfg["corpus_files"]:
            path = corpus / name
            with self.subTest(file=name):
                self.assertTrue(path.is_file(), f"missing {path}")
                raw = path.read_bytes()
                zframe = _pure_zstd22(raw, level)
                bav = bav_compress(raw, method=method)
                self.assertEqual(zstd.ZstdDecompressor().decompress(zframe), raw)
                self.assertEqual(bav_decompress(bav), raw)
                self.assertLess(
                    len(bav),
                    len(zframe),
                    f"{name}: BAV {len(bav)} did not beat pure Zstd-22 {len(zframe)}",
                )
                z_total += len(zframe)
                bav_total += len(bav)

        self.assertLess(bav_total, z_total)


if __name__ == "__main__":
    unittest.main()
