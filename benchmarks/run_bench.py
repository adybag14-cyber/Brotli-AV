#!/usr/bin/env python3
"""
Benchmark harness: new BAV vs frozen prior-BAV (primary), with Zstd-22 secondary.

Uses the real shipped bav.compress path. Emits self-beat + frontier-oriented
JSON/text reports (entropy estimates live in run_frontier.py).
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


def load_config() -> dict:
    return json.loads((Path(__file__).with_name("config.json")).read_text(encoding="utf-8"))


def load_prior_baseline() -> dict:
    path = ROOT / "benchmarks" / "prior_bav_baseline.json"
    return json.loads(path.read_text(encoding="utf-8"))


def zstd22_compress(data: bytes, level: int = 22) -> bytes:
    return zstd.ZstdCompressor(level=level).compress(data)


def bench_file(path: Path, frozen_size: int, method: str, zstd_level: int) -> dict:
    raw = path.read_bytes()
    t0 = time.perf_counter()
    bav = bav_compress(raw, method=method)
    t_bav = time.perf_counter() - t0
    t0 = time.perf_counter()
    zframe = zstd22_compress(raw, zstd_level)
    t_z = time.perf_counter() - t0

    if bav_decompress(bav) != raw:
        raise AssertionError(f"bav round-trip failed: {path.name}")
    if zstd.ZstdDecompressor().decompress(zframe) != raw:
        raise AssertionError(f"zstd round-trip failed: {path.name}")

    return {
        "file": path.name,
        "uncompressed": len(raw),
        "frozen_bav_size": frozen_size,
        "bav_size": len(bav),
        "zstd22_size": len(zframe),
        "delta_vs_frozen": frozen_size - len(bav),
        "delta_vs_zstd22": len(zframe) - len(bav),
        "beats_frozen": len(bav) < frozen_size,
        "beats_zstd22": len(bav) < len(zframe),
        "bav_compress_s": round(t_bav, 6),
        "zstd22_compress_s": round(t_z, 6),
    }


def format_text(report: dict) -> str:
    lines = []
    lines.append("Brotli-AV vs frozen prior-BAV — self-beat report")
    lines.append("=" * 60)
    lines.append(f"baseline: {report['baseline']['name']}")
    lines.append(f"research: {report['research']['name']} method={report['research']['method']}")
    lines.append("")
    lines.append(
        f"{'file':28} {'raw':>8} {'frozen':>8} {'new':>8} {'delta':>8} {'win':>5}"
    )
    lines.append("-" * 72)
    for r in report["files"]:
        win = "NEW" if r["beats_frozen"] else ("TIE" if r["bav_size"] == r["frozen_bav_size"] else "OLD")
        lines.append(
            f"{r['file']:28} {r['uncompressed']:8} {r['frozen_bav_size']:8} "
            f"{r['bav_size']:8} {r['delta_vs_frozen']:8} {win:>5}"
        )
    lines.append("-" * 72)
    t = report["totals"]
    lines.append(
        f"{'TOTAL':28} {t['uncompressed']:8} {t['frozen_bav_size']:8} "
        f"{t['bav_size']:8} {t['delta_vs_frozen']:8}"
    )
    lines.append("")
    lines.append(f"new_total:                      {t['bav_size']}")
    lines.append(f"frozen_prior_bav_total:         {t['frozen_bav_size']}")
    lines.append(f"beats_frozen_total:             {t['bav_size'] < t['frozen_bav_size']}")
    lines.append(f"beats_frozen_every_file:        {report['beats_every_file']}")
    lines.append(f"beats_zstd22_every_file:        {report['beats_zstd22_every_file']}")
    lines.append(f"all_roundtrips_ok:              {report['all_roundtrips_ok']}")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="BAV vs frozen prior-BAV self-beat benchmark")
    ap.add_argument("-o", "--output", help="JSON report path (.txt sibling if .json)")
    ap.add_argument("--text-output", help="plain-text report path")
    args = ap.parse_args(argv)

    cfg = load_config()
    prior = load_prior_baseline()
    corpus = ROOT / cfg["corpus_dir"]
    method = cfg["research"]["method"]
    zstd_level = int(cfg.get("secondary_baseline_zstd", {}).get("level", 22))

    files = []
    for name in cfg["corpus_files"]:
        path = corpus / name
        if not path.is_file():
            raise FileNotFoundError(f"missing corpus file: {path}")
        if name not in prior["files"]:
            raise KeyError(f"missing frozen size for {name}")
        files.append(bench_file(path, int(prior["files"][name]), method, zstd_level))

    totals = {
        "uncompressed": sum(r["uncompressed"] for r in files),
        "frozen_bav_size": sum(r["frozen_bav_size"] for r in files),
        "bav_size": sum(r["bav_size"] for r in files),
        "zstd22_size": sum(r["zstd22_size"] for r in files),
        "delta_vs_frozen": sum(r["delta_vs_frozen"] for r in files),
    }
    beats_every = all(r["beats_frozen"] for r in files)
    beats_z = all(r["beats_zstd22"] for r in files)
    report = {
        "baseline": cfg["baseline"],
        "prior_bav_baseline": {
            "total": prior["total"],
            "files": prior["files"],
        },
        "research": cfg["research"],
        "files": files,
        "totals": totals,
        "all_roundtrips_ok": True,
        "beats_every_file": beats_every,
        "beats_zstd22_every_file": beats_z,
        "research_beats_prior_bav": totals["bav_size"] < totals["frozen_bav_size"] and beats_every,
    }

    text = format_text(report)
    print(text, end="")

    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        if out.suffix.lower() == ".json":
            out.with_suffix(".txt").write_text(text, encoding="utf-8")
        print(f"wrote {out}")
    if args.text_output:
        tp = Path(args.text_output)
        tp.parent.mkdir(parents=True, exist_ok=True)
        tp.write_text(text, encoding="utf-8")
        print(f"wrote {tp}")

    if not beats_every or totals["bav_size"] >= totals["frozen_bav_size"]:
        print("FAIL: did not strictly beat frozen prior-BAV on every file and total", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
