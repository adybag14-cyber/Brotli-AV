#!/usr/bin/env python3
"""
Scientific-frontier report: sizes vs frozen prior-BAV and entropy lower-bound estimates.

Not a claim of Kolmogorov optimality. Order-0 / order-1 Shannon estimates are
documented reference bounds; residual gap = new_size − bound (may be negative
when structure beats memoryless entropy).
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import zstandard as zstd  # noqa: E402
from bav.codec import compress as bav_compress  # noqa: E402
from bav.codec import decompress as bav_decompress  # noqa: E402


def load_config() -> dict:
    return json.loads((ROOT / "benchmarks" / "config.json").read_text(encoding="utf-8"))


def load_prior() -> dict:
    return json.loads((ROOT / "benchmarks" / "prior_bav_baseline.json").read_text(encoding="utf-8"))


def order0_entropy_bytes(data: bytes) -> float:
    """Shannon order-0 entropy in bytes (H0 * n / 8)."""
    n = len(data)
    if n == 0:
        return 0.0
    counts = Counter(data)
    h = 0.0
    for c in counts.values():
        p = c / n
        h -= p * math.log2(p)
    return h * n / 8.0


def order1_entropy_bytes(data: bytes) -> float:
    """
    Empirical order-1 conditional entropy in bytes:
    H(X_i | X_{i-1}) * (n-1)/8 + 8 bits for the first symbol (upper-ish ref).
    """
    n = len(data)
    if n == 0:
        return 0.0
    if n == 1:
        return 1.0
    # joint counts of (prev, cur)
    joint: dict[int, Counter] = {}
    prev_counts: Counter = Counter()
    for i in range(1, n):
        p, c = data[i - 1], data[i]
        prev_counts[p] += 1
        joint.setdefault(p, Counter())[c] += 1
    h_bits = 0.0
    for p, total in prev_counts.items():
        for cnt in joint[p].values():
            prob = cnt / total
            h_bits -= total * (prob * math.log2(prob))
    # first byte: 8 bits worst case (or order-0 of single symbol = 0 if known)
    h_bits += 8.0
    return h_bits / 8.0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="BAV frontier entropy/size report")
    ap.add_argument("-o", "--output", required=True, help="JSON output path")
    args = ap.parse_args(argv)

    cfg = load_config()
    prior = load_prior()
    method = cfg["research"]["method"]
    corpus = ROOT / cfg["corpus_dir"]

    rows = []
    for name in cfg["corpus_files"]:
        path = corpus / name
        raw = path.read_bytes()
        frozen = int(prior["files"][name])
        frame = bav_compress(raw, method=method)
        if bav_decompress(frame) != raw:
            raise AssertionError(f"round-trip failed: {name}")
        zframe = zstd.ZstdCompressor(level=22).compress(raw)
        h0 = order0_entropy_bytes(raw)
        h1 = order1_entropy_bytes(raw)
        new_sz = len(frame)
        rows.append(
            {
                "file": name,
                "uncompressed": len(raw),
                "frozen_bav_size": frozen,
                "new_bav_size": new_sz,
                "zstd22_size": len(zframe),
                "order0_entropy_bytes": round(h0, 4),
                "order1_entropy_bytes": round(h1, 4),
                "residual_gap_vs_order0": round(new_sz - h0, 4),
                "residual_gap_vs_order1": round(new_sz - h1, 4),
                "delta_vs_frozen": frozen - new_sz,
                "beats_frozen": new_sz < frozen,
                "beats_zstd22": new_sz < len(zframe),
            }
        )

    totals = {
        "uncompressed": sum(r["uncompressed"] for r in rows),
        "frozen_bav_size": sum(r["frozen_bav_size"] for r in rows),
        "new_bav_size": sum(r["new_bav_size"] for r in rows),
        "zstd22_size": sum(r["zstd22_size"] for r in rows),
        "order0_entropy_bytes": round(sum(r["order0_entropy_bytes"] for r in rows), 4),
        "order1_entropy_bytes": round(sum(r["order1_entropy_bytes"] for r in rows), 4),
        "residual_gap_vs_order0": round(sum(r["residual_gap_vs_order0"] for r in rows), 4),
        "residual_gap_vs_order1": round(sum(r["residual_gap_vs_order1"] for r in rows), 4),
        "delta_vs_frozen": sum(r["delta_vs_frozen"] for r in rows),
    }
    report = {
        "title": "Brotli-AV scientific frontier size/entropy report",
        "disclaimer": (
            "Order-0/1 Shannon estimates are reference bounds, not Kolmogorov complexity. "
            "Negative residual gaps mean the compressor exploited structure beyond memoryless models."
        ),
        "primary_gate": "new_bav_size < frozen_bav_size for every file and total",
        "prior_bav_baseline": prior["files"],
        "prior_bav_total": prior["total"],
        "research_method": method,
        "files": rows,
        "totals": totals,
        "beats_frozen_every_file": all(r["beats_frozen"] for r in rows),
        "beats_frozen_total": totals["new_bav_size"] < totals["frozen_bav_size"],
    }

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    # text sibling
    lines = [
        report["title"],
        "=" * 60,
        report["disclaimer"],
        "",
        f"{'file':28} {'raw':>8} {'frozen':>8} {'new':>8} {'H0':>10} {'H1':>10} {'gapH1':>10}",
        "-" * 90,
    ]
    for r in rows:
        lines.append(
            f"{r['file']:28} {r['uncompressed']:8} {r['frozen_bav_size']:8} "
            f"{r['new_bav_size']:8} {r['order0_entropy_bytes']:10.1f} "
            f"{r['order1_entropy_bytes']:10.1f} {r['residual_gap_vs_order1']:10.1f}"
        )
    lines.append("-" * 90)
    lines.append(
        f"{'TOTAL':28} {totals['uncompressed']:8} {totals['frozen_bav_size']:8} "
        f"{totals['new_bav_size']:8} {totals['order0_entropy_bytes']:10.1f} "
        f"{totals['order1_entropy_bytes']:10.1f} {totals['residual_gap_vs_order1']:10.1f}"
    )
    lines.append("")
    lines.append(f"beats_frozen_every_file: {report['beats_frozen_every_file']}")
    lines.append(f"beats_frozen_total:      {report['beats_frozen_total']}")
    text = "\n".join(lines) + "\n"
    out.with_suffix(".txt").write_text(text, encoding="utf-8")
    print(text, end="")
    print(f"wrote {out}")

    if not report["beats_frozen_every_file"] or not report["beats_frozen_total"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
