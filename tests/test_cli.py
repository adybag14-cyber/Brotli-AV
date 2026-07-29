#!/usr/bin/env python3
"""CLI entry-point tests — drive real python -m bav compress/decompress."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
CORPUS = ROOT / "corpus" / "01_plain_text.txt"


class TestCLI(unittest.TestCase):
    def _env(self) -> dict:
        import os

        env = os.environ.copy()
        # Prefer editable install; also allow plain path injection
        env["PYTHONPATH"] = str(SRC) + os.pathsep + env.get("PYTHONPATH", "")
        return env

    def test_compress_decompress_roundtrip_cli(self):
        self.assertTrue(CORPUS.is_file(), "corpus missing; run tools/generate_corpus.py")
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            out_bav = td_path / "out.bav"
            out_raw = td_path / "out.bin"
            r1 = subprocess.run(
                [sys.executable, "-m", "bav", "compress", str(CORPUS), "-o", str(out_bav)],
                capture_output=True,
                text=True,
                env=self._env(),
                cwd=str(ROOT),
            )
            self.assertEqual(r1.returncode, 0, r1.stderr + r1.stdout)
            self.assertTrue(out_bav.is_file())
            self.assertGreater(out_bav.stat().st_size, 0)

            r2 = subprocess.run(
                [sys.executable, "-m", "bav", "decompress", str(out_bav), "-o", str(out_raw)],
                capture_output=True,
                text=True,
                env=self._env(),
                cwd=str(ROOT),
            )
            self.assertEqual(r2.returncode, 0, r2.stderr + r2.stdout)
            self.assertEqual(out_raw.read_bytes(), CORPUS.read_bytes())

    def test_compress_twice_identical_size(self):
        self.assertTrue(CORPUS.is_file())
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            a = td_path / "a.bav"
            b = td_path / "b.bav"
            for dest in (a, b):
                r = subprocess.run(
                    [sys.executable, "-m", "bav", "compress", str(CORPUS), "-o", str(dest)],
                    capture_output=True,
                    text=True,
                    env=self._env(),
                    cwd=str(ROOT),
                )
                self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
            self.assertEqual(a.stat().st_size, b.stat().st_size)
            self.assertEqual(a.read_bytes(), b.read_bytes())


if __name__ == "__main__":
    unittest.main()
