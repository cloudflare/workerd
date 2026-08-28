# R2-SDK stream consumption patterns

R2's SDK consumes workerd streams through readAtLeast-driven BYOB
loops, tees, Request clones, and TextDecoderStream. This suite pins
those real-world shapes against both implementations, migrated
wholesale from streams-r2-patterns-test.js. **The tests are the
normative artifact.**

## Divergence ledger (C++ vs TypeScript)

The TS side of every entry is a bounded defect pin (the readable-byte
suite's ledger #5/#6 and #12 families surfacing in R2's exact usage) —
this suite is effectively the R2 to-do list for the TypeScript streams
migration.

| # | Pattern | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | readAtLeast(min) when close arrives below min | folds the available bytes into a done=false result, then a done read | PENDS FOREVER (bounded) | `byobReadAtLeastAutomatic`, `closedByobTeeOnStart` (both tee branches) |
| 2 | tee pulls and `c.byobRequest` | tee pulls carry a byobRequest (atLeast/freshness asserted when present) | tee-driven pulls present a NULL byobRequest — ACCEPTED DIVERGENCE (decided 2026-08-28): sources must be null-tolerant (check `c.byobRequest`, enqueue() as the fallback); with that supported pattern the tee shapes are PARITY except the tail-shape flag below | `byobReadAtLeastTee`, `byobReadAtLeastTeeComplex1/2/3` (all four demonstrate the supported dual-path source) |
| 3 | identity-stream readAtLeast tail | trailing below-min remainder delivered done=false, then 0-length done — the DECIDED contract | view-exact fills, then the below-min tail PENDS FOREVER at close (ts backlog P0 #1); the fold-done variant appears in the tee tests (ts backlog #24) | `identityTransformStreamReadAtLeast`, tee complex variants (r3 done flag) |
| 4 | chained identity→byte-stream readAtLeast consumption (the R2 body pump shape) | full 5000-byte body in 102-byte minimums | 49 full reads (4998 bytes), then the 2-byte tail PENDS | `partiallyFilledByobAtLeast` |

Parity worth noting: manual `byobRequest.atLeast` handling on a DIRECT
byob reader (`byobReadAtLeastManual` — byobRequest is synthesized for
direct reads under both), FixedLengthStream readAtLeast
(`fixedLengthStreamReadAtLeast`), plain identity readAtLeast
(`identityTransformReadAtLeast`), Request clone BYOB consumption
(`requestCloneByob`), TextDecoderStream over a Request body
(`textDecoderStreamRequest`).
