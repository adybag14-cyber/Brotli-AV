#!/usr/bin/env python3
"""
Benchmark harness: research BAV compressor vs pure Zstd level 22 (primary).

Uses the real shipped bav.compress and zstandard ZstdCompressor(level=22).
Compares against raw Zstd frames (not BAV-wrapped) so header overhead cannot
fake a win. Optional secondary Brotli column when brotli is installed.

Writes a machine-readable report (JSON + text summary).
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import zstandard as zstd  # noqa: E402
from bav.codec import compress as bav_compress  # noqa: E402
from bav.codec import decompress as bav_decompress  # noqa: E402

try:
    import brotli as _brotli_mod
except ImportError:  # pragma: no cover
    _brotli_mod = None


def load_config() -> dict:
    cfg_path = Path(__file__).with_name("config.json")
    return json.loads(cfg_path.read_text(encoding="utf-8"))


def zstd22_compress(data: bytes, level: int = 22) -> bytes:
    """Pure Zstd frame at the documented baseline level (no BAV wrapper)."""
    return zstd.ZstdCompressor(level=level).compress(data)


def zstd22_decompress(frame: bytes) -> bytes:
    return zstd.ZstdDecompressor().decompress(frame)


def bench_file(path: Path, level: int, method: str, include_brotli: bool) -> dict:
    raw = path.read_bytes()
    t0 = time.perf_counter()
    zframe = zstd22_compress(raw, level)
    t_z = time.perf_counter() - t0
    t0 = time.perf_counter()
    bav = bav_compress(raw, method=method)
    t_bav = time.perf_counter() - t0

    if zstd22_decompress(zframe) != raw:
        raise AssertionError(f"zstd round-trip failed: {path.name}")
    if bav_decompress(bav) != raw:
        raise AssertionError(f"bav round-trip failed: {path.name}")

    row = {
        "file": path.name,
        "uncompressed": len(raw),
        "zstd22_size": len(zframe),
        "bav_size": len(bav),
        "zstd22_ratio": (len(zframe) / len(raw)) if raw else 0.0,
        "bav_ratio": (len(bav) / len(raw)) if raw else 0.0,
        "zstd22_compress_s": round(t_z, 6),
        "bav_compress_s": round(t_bav, 6),
        "bav_wins": len(bav) < len(zframe),
        "delta_bytes": len(zframe) - len(bav),
    }
    if include_brotli and _brotli_mod is not None:
        br = _brotli_mod.compress(raw, quality=11)
        if _brotli_mod.decompress(br) != raw:
            raise AssertionError(f"brotli round-trip failed: {path.name}")
        row["brotli11_size"] = len(br)
    return row


def format_text(report: dict) -> str:
    lines = []
    lines.append("Brotli-AV vs Zstd-22 — benchmark report")
    lines.append("=" * 60)
    b = report["baseline"]
    lines.append(
        f"baseline: {b['name']} level={b.get('level', 22)} pin={b.get('version_pin')}"
    )
    lines.append(
        f"research: {report['research']['name']} method={report['research']['method']}"
    )
    lines.append("note: Zstd-22 sizes are pure zstd frames (not BAV-wrapped)")
    lines.append("")
    lines.append(
        f"{'file':28} {'raw':>8} {'zstd22':>8} {'bav':>8} {'delta':>8} {'win':>5}"
    )
    lines.append("-" * 70)
    for r in report["files"]:
        win = "BAV" if r["bav_wins"] else (
            "TIE" if r["bav_size"] == r["zstd22_size"] else "ZSTD"
        )
        lines.append(
            f"{r['file']:28} {r['uncompressed']:8} {r['zstd22_size']:8} "
            f"{r['bav_size']:8} {r['delta_bytes']:8} {win:>5}"
        )
    lines.append("-" * 70)
    t = report["totals"]
    lines.append(
        f"{'TOTAL':28} {t['uncompressed']:8} {t['zstd22_size']:8} "
        f"{t['bav_size']:8} {t['delta_bytes']:8}"
    )
    lines.append("")
    lines.append(f"research_total_compressed_bytes: {t['bav_size']}")
    lines.append(f"zstd22_total_compressed_bytes:   {t['zstd22_size']}")
    lines.append(f"research_beats_zstd22_total:     {t['bav_size'] < t['zstd22_size']}")
    lines.append(f"research_beats_zstd22_every_file:{report['beats_every_file']}")
    lines.append(f"all_roundtrips_ok:               {report['all_roundtrips_ok']}")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="BAV vs pure Zstd-22 benchmark")
    ap.add_argument(
        "-o",
        "--output",
        help="write JSON report to this path (also writes .txt sibling if ends with .json)",
    )
    ap.add_argument(
        "--text-output",
        help="write plain-text report to this path",
    )
    ap.add_argument(
        "--with-brotli",
        action="store_true",
        help="also record Brotli q=11 sizes (secondary, not the gate)",
    )
    args = ap.parse_args(argv)

    cfg = load_config()
    corpus = ROOT / cfg["corpus_dir"]
    level = int(cfg["baseline"].get("level", 22))
    method = cfg["research"]["method"]

    files = []
    for name in cfg["corpus_files"]:
        path = corpus / name
        if not path.is_file():
            raise FileNotFoundError(f"missing corpus file: {path}")
        files.append(bench_file(path, level, method, args.with_brotli))

    totals = {
        "uncompressed": sum(r["uncompressed"] for r in files),
        "zstd22_size": sum(r["zstd22_size"] for r in files),
        "bav_size": sum(r["bav_size"] for r in files),
        "delta_bytes": sum(r["delta_bytes"] for r in files),
    }
    beats_every = all(r["bav_size"] < r["zstd22_size"] for r in files)
    report = {
        "baseline": cfg["baseline"],
        "research": cfg["research"],
        "files": files,
        "totals": totals,
        "all_roundtrips_ok": True,
        "beats_every_file": beats_every,
        "research_beats_zstd22": totals["bav_size"] < totals["zstd22_size"] and beats_every,
    }

    text = format_text(report)
    print(text, end="")

    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        if out.suffix.lower() == ".json":
            txt_path = out.with_suffix(".txt")
            txt_path.write_text(text, encoding="utf-8")
        print(f"wrote {out}")
    if args.text_output:
        tp = Path(args.text_output)
        tp.parent.mkdir(parents=True, exist_ok=True)
        tp.write_text(text, encoding="utf-8")
        print(f"wrote {tp}")

    if not beats_every or totals["bav_size"] >= totals["zstd22_size"]:
        print(
            "FAIL: research did not strictly beat pure Zstd-22 on every file and total",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
