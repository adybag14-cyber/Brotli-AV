"""BAV command-line entry point."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import __version__
from .codec import compress, compress_file, decompress, decompress_file


def _cmd_compress(args: argparse.Namespace) -> int:
    data = Path(args.input).read_bytes()
    frame = compress(data, method=args.method)
    out = Path(args.output) if args.output else Path(args.input).with_suffix(
        Path(args.input).suffix + ".bav"
    )
    out.write_bytes(frame)
    print(f"compressed {len(data)} -> {len(frame)} bytes ({out})")
    return 0


def _cmd_decompress(args: argparse.Namespace) -> int:
    frame = Path(args.input).read_bytes()
    data = decompress(frame)
    if args.output:
        out = Path(args.output)
    else:
        p = Path(args.input)
        out = p.with_suffix("") if p.suffix == ".bav" else Path(str(p) + ".out")
    out.write_bytes(data)
    print(f"decompressed {len(frame)} -> {len(data)} bytes ({out})")
    return 0


def _cmd_version(_: argparse.Namespace) -> int:
    print(f"bav {__version__}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="bav",
        description="Brotli-AV research compressor — beat Google Brotli on ratio",
    )
    p.add_argument("--version", action="store_true", help="print version and exit")
    sub = p.add_subparsers(dest="command")

    c = sub.add_parser("compress", help="compress a file to BAV1")
    c.add_argument("input", help="input file path")
    c.add_argument("-o", "--output", help="output .bav path")
    c.add_argument(
        "-m",
        "--method",
        default="auto",
        choices=["auto", "store", "deflate", "lzma", "zstd", "brotli", "research"],
        help="compression method (default: auto)",
    )
    c.set_defaults(func=_cmd_compress)

    d = sub.add_parser("decompress", help="decompress a BAV1 file")
    d.add_argument("input", help="input .bav path")
    d.add_argument("-o", "--output", help="output path")
    d.set_defaults(func=_cmd_decompress)

    v = sub.add_parser("version", help="print version")
    v.set_defaults(func=_cmd_version)

    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if getattr(args, "version", False) and not getattr(args, "command", None):
        print(f"bav {__version__}")
        return 0
    if not getattr(args, "command", None):
        parser.print_help()
        return 2
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main())
