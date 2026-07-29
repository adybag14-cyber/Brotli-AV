#!/usr/bin/env python3
"""Lossless round-trip tests against the shipped bav.compress / bav.decompress APIs."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from bav.codec import compress, decompress  # noqa: E402


def corpus_files(include_edges: bool = True) -> list[Path]:
    cfg = json.loads((ROOT / "benchmarks" / "config.json").read_text(encoding="utf-8"))
    names = list(cfg["corpus_files"])
    if include_edges:
        names.extend(cfg.get("edge_files", []))
    corpus = ROOT / "corpus"
    return [corpus / n for n in names]


class TestLossless(unittest.TestCase):
    def test_roundtrip_corpus(self):
        for path in corpus_files(include_edges=True):
            with self.subTest(file=path.name):
                self.assertTrue(path.is_file(), f"missing {path}")
                raw = path.read_bytes()
                frame = compress(raw, method="auto")
                out = decompress(frame)
                self.assertEqual(out, raw, f"round-trip mismatch: {path.name}")
                self.assertGreaterEqual(len(frame), 15)  # BAV v2 header always present

    def test_roundtrip_empty(self):
        raw = b""
        frame = compress(raw)
        self.assertEqual(decompress(frame), raw)

    def test_roundtrip_small(self):
        for raw in (b"x", b"\x00\x01\x02", b"Hello, Brotli-AV!\n" * 3):
            with self.subTest(raw=raw[:20]):
                self.assertEqual(decompress(compress(raw)), raw)

    def test_forced_methods(self):
        raw = b"abc" * 500
        for method in ("store", "deflate", "lzma", "zstd", "brotli", "auto", "research"):
            with self.subTest(method=method):
                self.assertEqual(decompress(compress(raw, method=method)), raw)

    def test_corrupt_crc_detected(self):
        frame = bytearray(compress(b"payload-data-12345"))
        # Flip a payload byte after the 18-byte header
        if len(frame) > 20:
            frame[-1] ^= 0xFF
            with self.assertRaises(ValueError):
                decompress(bytes(frame))


if __name__ == "__main__":
    unittest.main()
