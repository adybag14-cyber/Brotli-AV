#!/usr/bin/env python3
"""
Benchmark harness: research BAV compressor vs stock Google Brotli.

Uses the real shipped bav.compress / brotli.compress entry points.
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

import brotli  # noqa: E402
from bav.codec import compress as bav_compress  # noqa: E402
from bav.codec import decompress as bav_decompress  # noqa: E402


def load_config() -> dict:
    cfg_path = Path(__file__).with_name("config.json")
    return json.loads(cfg_path.read_text(encoding="utf-8"))


def brotli_compress(data: bytes, quality: int) -> bytes:
    return brotli.compress(data, quality=quality)


def brotli_decompress(data: bytes) -> bytes:
    return brotli.decompress(data)


def bench_file(path: Path, quality: int, method: str) -> dict:
    raw = path.read_bytes()
    t0 = time.perf_counter()
    br = brotli_compress(raw, quality)
    t_br = time.perf_counter() - t0
    t0 = time.perf_counter()
    bav = bav_compress(raw, method=method)
    t_bav = time.perf_counter() - t0

    # Lossless checks on the real codecs
    if brotli_decompress(br) != raw:
        raise AssertionError(f"brotli round-trip failed: {path.name}")
    if bav_decompress(bav) != raw:
        raise AssertionError(f"bav round-trip failed: {path.name}")

    return {
        "file": path.name,
        "uncompressed": len(raw),
        "brotli_size": len(br),
        "bav_size": len(bav),
        "brotli_ratio": (len(br) / len(raw)) if raw else 0.0,
        "bav_ratio": (len(bav) / len(raw)) if raw else 0.0,
        "brotli_compress_s": round(t_br, 6),
        "bav_compress_s": round(t_bav, 6),
        "bav_wins": len(bav) < len(br),
        "delta_bytes": len(br) - len(bav),
    }


def format_text(report: dict) -> str:
    lines = []
    lines.append("Brotli-AV vs Google Brotli — benchmark report")
    lines.append("=" * 60)
    lines.append(f"baseline: {report['baseline']['name']} quality={report['baseline']['quality']} pin={report['baseline']['version_pin']}")
    lines.append(f"research: {report['research']['name']} method={report['research']['method']}")
    lines.append("")
    lines.append(
        f"{'file':28} {'raw':>8} {'brotli':>8} {'bav':>8} {'delta':>8} {'win':>5}"
    )
    lines.append("-" * 70)
    for r in report["files"]:
        win = "BAV" if r["bav_wins"] else ("TIE" if r["bav_size"] == r["brotli_size"] else "BR")
        lines.append(
            f"{r['file']:28} {r['uncompressed']:8} {r['brotli_size']:8} "
            f"{r['bav_size']:8} {r['delta_bytes']:8} {win:>5}"
        )
    lines.append("-" * 70)
    t = report["totals"]
    lines.append(
        f"{'TOTAL':28} {t['uncompressed']:8} {t['brotli_size']:8} "
        f"{t['bav_size']:8} {t['delta_bytes']:8}"
    )
    lines.append("")
    lines.append(f"research_total_compressed_bytes: {t['bav_size']}")
    lines.append(f"brotli_total_compressed_bytes:   {t['brotli_size']}")
    lines.append(f"research_beats_brotli:           {t['bav_size'] < t['brotli_size']}")
    lines.append(f"all_roundtrips_ok:               {report['all_roundtrips_ok']}")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="BAV vs Google Brotli benchmark")
    ap.add_argument(
        "-o",
        "--output",
        help="write JSON report to this path (also writes .txt sibling if ends with .json)",
    )
    ap.add_argument(
        "--text-output",
        help="write plain-text report to this path",
    )
    args = ap.parse_args(argv)

    cfg = load_config()
    corpus = ROOT / cfg["corpus_dir"]
    quality = int(cfg["baseline"]["quality"])
    method = cfg["research"]["method"]

    files = []
    for name in cfg["corpus_files"]:
        path = corpus / name
        if not path.is_file():
            raise FileNotFoundError(f"missing corpus file: {path}")
        files.append(bench_file(path, quality, method))

    totals = {
        "uncompressed": sum(r["uncompressed"] for r in files),
        "brotli_size": sum(r["brotli_size"] for r in files),
        "bav_size": sum(r["bav_size"] for r in files),
        "delta_bytes": sum(r["delta_bytes"] for r in files),
    }
    report = {
        "baseline": cfg["baseline"],
        "research": cfg["research"],
        "files": files,
        "totals": totals,
        "all_roundtrips_ok": True,
        "research_beats_brotli": totals["bav_size"] < totals["brotli_size"],
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

    if totals["bav_size"] >= totals["brotli_size"]:
        print("FAIL: research total compressed bytes did not beat stock Brotli", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
