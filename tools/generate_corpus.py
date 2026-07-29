#!/usr/bin/env python3
"""Generate the fixed multi-type verification corpus (deterministic)."""

from __future__ import annotations

import random
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "corpus"


def write(name: str, data: bytes) -> None:
    path = CORPUS / name
    path.write_bytes(data)
    print(f"  {name}: {len(data)} bytes")


def plain_text() -> bytes:
    words = (
        b"the of and to a in that is was he for it with as his on be at by I this "
        b"had not are but from or have an they which one you were all her she there "
        b"would their we him been has when who will more no if out so said what up "
        b"its about into than them can only other new some could time these two may "
        b"then do first any now should compression research brotli algorithm dictionary "
        b"entropy model match finder window distance length literal context"
    ).split()
    rng = random.Random(0xBA71C0)
    lines = []
    for _ in range(400):
        line = b" ".join(words[rng.randrange(len(words))] for _ in range(rng.randint(8, 28)))
        lines.append(line)
    body = b"\n".join(lines) + b"\n"
    # Repeat with mild variation for longer stream
    return body + body[::-1][: len(body) // 3] + body


def html_js() -> bytes:
    head = b"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<title>Brotli-AV Dashboard</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:1rem;background:#0f1115;color:#e8eaed}
.card{border:1px solid #333;border-radius:8px;padding:12px;margin:8px 0}
.btn{background:#1a73e8;color:#fff;border:0;padding:6px 12px;border-radius:4px;cursor:pointer}
</style>
<script>
async function loadItems(){
  const r = await fetch('/api/v1/items');
  const data = await r.json();
  console.log('items', data.length);
  return data.map(x => ({id:x.id, name:x.name, score:x.score||0}));
}
function select(id){ window.location.hash = '#item-'+id; }
</script>
</head>
<body>
<div id="app"><h1>Dashboard</h1>
"""
    chunks = [head]
    for i in range(150):
        chunks.append(
            f'<div class="card" data-id="{i}">'
            f'<span class="title">Item {i} — research sample</span> '
            f'<button class="btn" onclick="select({i})">Select</button>'
            f'<p>Description for item {i} with repeated template text for compressibility.</p>'
            f"</div>\n".encode()
        )
    chunks.append(b"</div></body></html>\n")
    return b"".join(chunks)


def binary_records() -> bytes:
    buf = bytearray()
    for i in range(2500):
        buf += struct.pack("<IHH", i, i % 65535, (i * 7) % 65535)
        buf += bytes([(i * 3) & 0xFF, (i >> 3) & 0xFF, 0x00, 0xFF])
    # Add a small incompressible-ish tail
    rng = random.Random(99)
    buf += bytes(rng.getrandbits(8) for _ in range(512))
    return bytes(buf)


def mixed_archive() -> bytes:
    """Small mixed archive-style blob: pseudo-zip headers + text + html + binary."""
    text = plain_text()[:4000]
    html = html_js()[:5000]
    binary = binary_records()[:3000]
    parts = [
        b"PK\x03\x04",
        struct.pack("<HHI", 20, 0, len(text)),
        b"README.txt\x00",
        text,
        b"PK\x03\x04",
        struct.pack("<HHI", 20, 0, len(html)),
        b"index.html\x00",
        html,
        b"PK\x03\x04",
        struct.pack("<HHI", 20, 0, len(binary)),
        b"data.bin\x00",
        binary,
        b"PK\x05\x06END\n",
    ]
    return b"".join(parts)


def source_code() -> bytes:
    parts = [
        b'#!/usr/bin/env python3\n"""Synthetic source corpus for Brotli-AV."""\n\n'
    ]
    for i in range(90):
        parts.append(
            f"""
def process_item_{i}(data, flags=0):
    result = []
    for idx, value in enumerate(data):
        if value is None:
            continue
        transformed = (value * {i + 1} + flags) & 0xFFFFFFFF
        result.append({{"idx": idx, "value": transformed, "tag": "item_{i}"}})
    return result


class Handler{i}:
    def __init__(self, name="handler_{i}"):
        self.name = name
        self.cache = {{}}

    def run(self, payload):
        key = tuple(payload) if isinstance(payload, list) else payload
        if key in self.cache:
            return self.cache[key]
        out = process_item_{i}(payload)
        self.cache[key] = out
        return out
""".encode()
        )
    return b"".join(parts)


def empty_and_tiny() -> None:
    write("edge_empty.bin", b"")
    write("edge_tiny.txt", b"x")
    write("edge_small.txt", b"Hello, Brotli-AV!\n" * 3)


def main() -> None:
    CORPUS.mkdir(parents=True, exist_ok=True)
    print(f"Generating fixed corpus under {CORPUS}")
    write("01_plain_text.txt", plain_text())
    write("02_html_js.html", html_js())
    write("03_binary_records.bin", binary_records())
    write("04_mixed_archive.bin", mixed_archive())
    write("05_source_code.py", source_code())
    empty_and_tiny()
    # Manifest for harness
    names = sorted(p.name for p in CORPUS.iterdir() if p.is_file())
    (CORPUS / "MANIFEST.txt").write_text("\n".join(names) + "\n", encoding="utf-8")
    print("MANIFEST:", ", ".join(names))


if __name__ == "__main__":
    main()
