# Stock Google Brotli baseline

| Field | Value |
|-------|--------|
| Project | [google/brotli](https://github.com/google/brotli) |
| Comparison vehicle | Official Python bindings from PyPI |
| **Pinned version** | **`brotli==1.2.0`** |
| Quality flag | **`quality=11`** (maximum compression) |
| Mode | Default generic mode (`brotli.compress(data, quality=11)`) |
| Documented in | `benchmarks/config.json`, `requirements.txt` |

## Why this pin

Quality 11 is the strongest stock Brotli setting commonly used for ratio comparisons. Pinning `1.2.0` keeps harness results reproducible across machines.

## Install

```powershell
python -m pip install brotli==1.2.0
python -c "import brotli; print(brotli)"
```

## Optional native CLI

If a system `brotli.exe` is present, it is **not** required for the gating harness; the Python bindings call the same libbrotli family and are the documented comparison path for this repo.
