#!/usr/bin/env python3
"""Multi-language BAV speed harness vs Python baseline.

Shells out to each real CLI the same way a user would. Writes
progress/lang-speed-report.{json,txt} and optional SCRATCH copy.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "corpus"
PROGRESS = ROOT / "progress"
DEFAULT_SCRATCH = Path(
    os.environ.get(
        "BAV_SCRATCH",
        r"C:\Users\adyba\AppData\Local\Temp\grok-goal-4352734f52d0\implementer",
    )
)

CORPUS_FILES = [
    "01_plain_text.txt",
    "02_html_js.html",
    "03_binary_records.bin",
    "04_mixed_archive.bin",
    "05_source_code.py",
    "edge_empty.bin",
    "edge_small.txt",
    "edge_tiny.txt",
]


def _which(name: str) -> str | None:
    return shutil.which(name)


def discover_impls() -> dict[str, dict]:
    """Map impl name -> {cmd_prefix list, env} for compress/decompress.

    Default production port is C# (listed first after discovery order
    preference: csharp, then python reference, then others).
    """
    impls: dict[str, dict] = {}

    # Default production: C# full research
    csharp = ROOT / "ports" / "csharp" / "bin" / "Release" / "net8.0" / "bav-csharp.exe"
    if not csharp.is_file():
        csharp = ROOT / "ports" / "csharp" / "bin" / "Release" / "net8.0" / "bav-csharp"
    if not csharp.is_file():
        alt = list((ROOT / "ports" / "csharp").rglob("bav-csharp.exe"))
        if alt:
            csharp = alt[0]
    if csharp.is_file():
        impls["csharp"] = {
            "compress": [str(csharp), "compress"],
            "decompress": [str(csharp), "decompress"],
            "default": True,
        }

    # Python reference
    impls["python"] = {
        "compress": [sys.executable, "-m", "bav", "compress"],
        "decompress": [sys.executable, "-m", "bav", "decompress"],
        "cwd": str(ROOT),
        "env": {**os.environ, "PYTHONPATH": str(ROOT / "src")},
    }

    rust = ROOT / "ports" / "rust" / "target" / "release" / "bav-rust.exe"
    if not rust.is_file():
        rust = ROOT / "ports" / "rust" / "target" / "release" / "bav-rust"
    if rust.is_file():
        impls["rust"] = {
            "compress": [str(rust), "compress"],
            "decompress": [str(rust), "decompress"],
        }

    zig_bin = ROOT / "ports" / "zig" / "zig-out" / "bin" / "bav-zig.exe"
    if not zig_bin.is_file():
        zig_bin = ROOT / "ports" / "zig" / "zig-out" / "bin" / "bav-zig"
    if zig_bin.is_file():
        impls["zig"] = {
            "compress": [str(zig_bin), "compress"],
            "decompress": [str(zig_bin), "decompress"],
        }

    for name, rel in [
        ("c", ROOT / "ports" / "c" / "bav-c.exe"),
        ("c_nasm", ROOT / "ports" / "c" / "bav-c-nasm.exe"),
        ("cpp", ROOT / "ports" / "cpp" / "bav-cpp.exe"),
        ("cpp_nasm", ROOT / "ports" / "cpp" / "bav-cpp-nasm.exe"),
    ]:
        p = rel
        if not p.is_file():
            p = Path(str(rel).replace(".exe", ""))
        if p.is_file():
            impls[name] = {
                "compress": [str(p), "compress"],
                "decompress": [str(p), "decompress"],
            }

    ruby = _which("ruby")
    ruby_script = ROOT / "ports" / "ruby" / "bav.rb"
    if ruby and ruby_script.is_file():
        impls["ruby"] = {
            "compress": [ruby, str(ruby_script), "compress"],
            "decompress": [ruby, str(ruby_script), "decompress"],
        }

    return impls


def run_cmd(cmd: list[str], *, cwd: str | None = None, env: dict | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=cwd,
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )


def bench_one(
    impl_name: str,
    impl: dict,
    corpus_path: Path,
    method: str,
    work: Path,
) -> dict:
    stem = corpus_path.name
    frame = work / f"{impl_name}_{stem}.bav"
    restored = work / f"{impl_name}_{stem}.out"
    env = impl.get("env")
    cwd = impl.get("cwd")

    ccmd = impl["compress"] + [str(corpus_path), "-o", str(frame), "-m", method]
    t0 = time.perf_counter()
    cr = run_cmd(ccmd, cwd=cwd, env=env)
    t1 = time.perf_counter()
    if cr.returncode != 0 or not frame.is_file():
        return {
            "impl": impl_name,
            "file": stem,
            "ok": False,
            "error": f"compress failed rc={cr.returncode} stderr={cr.stderr!r} stdout={cr.stdout!r}",
        }

    dcmd = impl["decompress"] + [str(frame), "-o", str(restored)]
    t2 = time.perf_counter()
    dr = run_cmd(dcmd, cwd=cwd, env=env)
    t3 = time.perf_counter()
    if dr.returncode != 0 or not restored.is_file():
        return {
            "impl": impl_name,
            "file": stem,
            "ok": False,
            "error": f"decompress failed rc={dr.returncode} stderr={dr.stderr!r}",
        }

    original = corpus_path.read_bytes()
    got = restored.read_bytes()
    lossless = original == got
    return {
        "impl": impl_name,
        "file": stem,
        "ok": lossless,
        "orig_size": len(original),
        "comp_size": frame.stat().st_size,
        "compress_s": t1 - t0,
        "decompress_s": t3 - t2,
        "compress_stdout": cr.stdout.strip(),
        "decompress_stdout": dr.stdout.strip(),
        "error": None if lossless else "round-trip mismatch",
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Multi-lang BAV speed bench")
    ap.add_argument("-m", "--method", default="auto", help="method passed to CLIs")
    ap.add_argument("--runs", type=int, default=2, help="full harness repetitions")
    ap.add_argument("--scratch", type=Path, default=DEFAULT_SCRATCH)
    args = ap.parse_args()

    # Ensure Python package importable
    src = str(ROOT / "src")
    if src not in sys.path:
        sys.path.insert(0, src)

    impls = discover_impls()
    if "python" not in impls:
        print("python baseline missing", file=sys.stderr)
        return 1

    all_rows: list[dict] = []
    with tempfile.TemporaryDirectory(prefix="bav-lang-bench-") as td:
        work = Path(td)
        for run_i in range(args.runs):
            for name, impl in impls.items():
                for fname in CORPUS_FILES:
                    path = CORPUS / fname
                    if not path.is_file():
                        continue
                    row = bench_one(name, impl, path, args.method, work)
                    row["run"] = run_i + 1
                    all_rows.append(row)
                    status = "OK" if row["ok"] else "FAIL"
                    cs = row.get("compress_s")
                    print(
                        f"[run {run_i+1}] {name:10} {fname:24} {status}"
                        + (f"  c={cs:.4f}s" if cs is not None else f"  {row.get('error')}")
                    )

    # Aggregate mean compress time for full corpus per impl (ok rows only)
    by_impl: dict[str, dict] = {}
    for row in all_rows:
        if not row["ok"]:
            continue
        d = by_impl.setdefault(
            row["impl"],
            {
                "compress_s_sum": 0.0,
                "decompress_s_sum": 0.0,
                "comp_size_sum": 0,
                "orig_size_sum": 0,
                "n": 0,
                "files": set(),
            },
        )
        d["compress_s_sum"] += row["compress_s"]
        d["decompress_s_sum"] += row["decompress_s"]
        d["comp_size_sum"] += row["comp_size"]
        d["orig_size_sum"] += row["orig_size"]
        d["n"] += 1
        d["files"].add(row["file"])

    summary = {}
    for name, d in by_impl.items():
        n = max(d["n"], 1)
        # per-run totals: divide by number of runs for mean full-corpus time
        runs = args.runs
        summary[name] = {
            "mean_full_corpus_compress_s": d["compress_s_sum"] / runs,
            "mean_full_corpus_decompress_s": d["decompress_s_sum"] / runs,
            "mean_total_comp_size": d["comp_size_sum"] / runs,
            "mean_total_orig_size": d["orig_size_sum"] / runs,
            "ok_measurements": d["n"],
            "files_ok": sorted(d["files"]),
        }

    py = summary.get("python", {})
    py_c = py.get("mean_full_corpus_compress_s")
    conclusions = {}
    for lang in ("rust", "zig"):
        if lang in summary and py_c:
            t = summary[lang]["mean_full_corpus_compress_s"]
            improved = t < py_c
            conclusions[lang] = {
                "improved_over_python": improved,
                "python_compress_s": py_c,
                f"{lang}_compress_s": t,
                "speedup_x": (py_c / t) if t > 0 else None,
                "note": (
                    f"YES — {lang} compress wall time improved over Python "
                    f"({t:.4f}s vs {py_c:.4f}s, {py_c/t:.2f}x)"
                    if improved
                    else f"NO — {lang} was not faster than Python "
                    f"({t:.4f}s vs {py_c:.4f}s)"
                ),
            }
        else:
            conclusions[lang] = {
                "improved_over_python": False,
                "note": f"{lang} missing from successful results",
            }

    report = {
        "method": args.method,
        "runs": args.runs,
        "impls_discovered": sorted(impls.keys()),
        "summary": summary,
        "conclusions": conclusions,
        "rows": all_rows,
        "format_interop_notes": (
            "Ports share BAV1 header layout (magic/version/method/orig/crc). "
            "Backend payloads for deflate use zlib; research transforms "
            "(MTF/RLE0/transpose/BWT) match Python semantics. "
            "Bit-identical frames across languages are not required."
        ),
    }

    PROGRESS.mkdir(parents=True, exist_ok=True)
    args.scratch.mkdir(parents=True, exist_ok=True)

    json_path = PROGRESS / "lang-speed-report.json"
    txt_path = PROGRESS / "lang-speed-report.txt"
    json_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    lines = [
        "BAV multi-language speed report",
        f"method={args.method} runs={args.runs}",
        f"impls: {', '.join(sorted(impls.keys()))}",
        "",
        "=== Per-impl mean full-corpus times ===",
    ]
    for name, s in sorted(summary.items()):
        lines.append(
            f"  {name:10} compress={s['mean_full_corpus_compress_s']:.4f}s "
            f"decompress={s['mean_full_corpus_decompress_s']:.4f}s "
            f"comp_bytes={s['mean_total_comp_size']:.0f} "
            f"orig_bytes={s['mean_total_orig_size']:.0f}"
        )
    lines.append("")
    lines.append("=== Rust / Zig improved over Python? ===")
    for lang in ("rust", "zig"):
        lines.append(f"  {lang}: {conclusions[lang]['note']}")
    lines.append("")
    fails = [r for r in all_rows if not r["ok"]]
    lines.append(f"failures: {len(fails)} / {len(all_rows)}")
    for r in fails[:20]:
        lines.append(f"  FAIL {r['impl']} {r['file']}: {r.get('error')}")

    txt = "\n".join(lines) + "\n"
    txt_path.write_text(txt, encoding="utf-8")

    (args.scratch / "bav-lang-speed-report.json").write_text(
        json.dumps(report, indent=2), encoding="utf-8"
    )
    (args.scratch / "bav-lang-speed-report.txt").write_text(txt, encoding="utf-8")

    print(txt)
    print(f"wrote {json_path}")
    print(f"wrote {args.scratch / 'bav-lang-speed-report.json'}")
    return 0 if not fails else 1


if __name__ == "__main__":
    raise SystemExit(main())
