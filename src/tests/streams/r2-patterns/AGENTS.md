# R2-SDK stream consumption patterns

R2's SDK consumes workerd streams through readAtLeast-driven BYOB
loops, tees, Request clones, and TextDecoderStream. This suite pins
those real-world shapes against both implementations, migrated
wholesale from streams-r2-patterns-test.js. **The tests are the
normative artifact.**

## Divergence ledger (C++ vs TypeScript)

The readAtLeast tail rows all follow the DECIDED contract (matching
C++): a close below the minimum folds the available bytes into a
done=false result and a follow-up read resolves done. The one remaining
divergence is the tee byobRequest model (row 2, accepted).

| # | Pattern | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | readAtLeast(min) when close arrives below min | folds the available bytes into a done=false result, then a done read | same (deferred end-of-data commit; readable-byte ledger #12/#19) | `byobReadAtLeastAutomatic`, `closedByobTeeOnStart` (both tee branches) |
| 2 | tee pulls and `c.byobRequest` | tee pulls carry a byobRequest (atLeast/freshness asserted when present) | tee-driven pulls present a NULL byobRequest — ACCEPTED DIVERGENCE (decided 2026-08-28): sources must be null-tolerant (check `c.byobRequest`, enqueue() as the fallback); with that supported pattern the tee shapes are PARITY, tail included | `byobReadAtLeastTee`, `byobReadAtLeastTeeComplex1/2/3` (all four demonstrate the supported dual-path source) |
| 3 | identity-stream readAtLeast tail | trailing below-min remainder delivered done=false, then 0-length done — the DECIDED contract | same | `identityTransformStreamReadAtLeast`, tee complex variants (r3 done flag) |
| 4 | chained identity→byte-stream readAtLeast consumption (the R2 body pump shape) | full 5000-byte body in 102-byte minimums | same | `partiallyFilledByobAtLeast` |

Parity worth noting: manual `byobRequest.atLeast` handling on a DIRECT
byob reader (`byobReadAtLeastManual` — byobRequest is synthesized for
direct reads under both), FixedLengthStream readAtLeast
(`fixedLengthStreamReadAtLeast`), plain identity readAtLeast
(`identityTransformReadAtLeast`), Request clone BYOB consumption
(`requestCloneByob`), TextDecoderStream over a Request body
(`textDecoderStreamRequest`).
