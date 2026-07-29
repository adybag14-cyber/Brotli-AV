# Comparison baselines

## Primary: Zstd level 22

| Field | Value |
|-------|--------|
| Engine | [facebook/zstd](https://github.com/facebook/zstd) via PyPI |
| **Package pin** | **`zstandard==0.25.0`** |
| **Level** | **22** (maximum) |
| API | `zstandard.ZstdCompressor(level=22).compress(data)` |
| Frame type | **Pure Zstd frames** (not BAV-wrapped) |
| Documented in | `benchmarks/config.json`, `requirements.txt` |

Gating win (this goal): research BAV1 compressed size is **strictly smaller than pure Zstd-22 on every fixed corpus file and on the total**.

```powershell
python -m pip install zstandard==0.25.0
python -c "import zstandard as z; print(z.__version__)"
```

## Secondary (historical): Google Brotli quality 11

| Field | Value |
|-------|--------|
| Package pin | `brotli==1.2.0` |
| Quality | 11 |
| Role | Prior goal (beat Brotli on **total** only); still covered by `tests/test_beat_brotli.py` |

## Install

```powershell
python -m pip install -r requirements.txt
```
