# Comparison baselines

## Primary: Frozen prior-BAV (self-beat bar)

| Field | Value |
|-------|--------|
| Source | `benchmarks/prior_bav_baseline.json` |
| Engine | Prior shipped BAV1 `auto` path at freeze time |
| **Total frozen** | **21364** bytes |
| Per-file | See JSON `files` map |
| Gate | New `bav.compress(..., method="auto")` size **&lt; frozen** on **every** corpus file and on the **total** |

This is the “BAV must beat BAV” bar. The freeze is a measured snapshot, not a re-implementation.

## Secondary: Zstd level 22

| Field | Value |
|-------|--------|
| Package pin | `zstandard==0.25.0` |
| Level | 22 |
| API | `ZstdCompressor(level=22).compress(data)` (pure frames) |
| Gate | Every file + total still strictly smaller (regression) |

## Secondary: Google Brotli quality 11

| Field | Value |
|-------|--------|
| Package pin | `brotli==1.2.0` |
| Quality | 11 |
| Gate | Total only (historical) |

## Install

```powershell
python -m pip install -r requirements.txt
```
