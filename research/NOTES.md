# Research notes — Brotli-AV

## Objective (current)

Strictly beat the **frozen prior-BAV** baseline on every fixed-corpus file and on the total, remain lossless, keep Zstd-22 every-file and Brotli-total regressions green, and publish a frontier size/entropy report (order-0/1 Shannon bounds + residual gaps).

## Why self-beat is hard

The freeze already took the min of prior backends (store/deflate/lzma/zstd/brotli + transpose + MTF/RLE0). Re-selecting the same candidates cannot win. Progress required **new modeling/transforms**.

## Advances in this generation

| Idea | Effect on fixed corpus |
|------|-------------------------|
| **XZ delta + LZMA2** distance search inside LZMA backend | Large win on structured binary (~1KB smaller on records) |
| **Full-file BWT** + MTF/RLE0 + backend | Wins on HTML, source, plain text margins |
| **Multi-block** best-backend | Helps heterogeneous / mixed blobs |
| Faster MTF (rank table) | Same ratios, less overhead in research paths |

## Frontier metrics

- `order0_entropy_bytes`: memoryless Shannon bound  
- `order1_entropy_bytes`: empirical H(X|prev) bound  
- `residual_gap_vs_order*`: `new_size − bound` (often **negative** when structure is exploited — expected, not a bug)

These are scientific **reference** numbers, not proofs of optimality.

## Future (not gating)

- Faster suffix array / SA-IS for BWT  
- Block-level BWT, PPM / context mixing, domain filters  
- External suites (LTCB, Squash) after self-beat stays green  
