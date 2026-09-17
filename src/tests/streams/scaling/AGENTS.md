# Queue scaling

Dequeue cost stays linear in every internal queue of both streams
implementations: buffered chunks, pending reads, pending pull-intos,
write requests, the writable controller's chunk queue, and the identity
stream's write snapshots. Each test drives one queue to 80k-160k
entries (identity: 80k, over a 16x range) and asserts, through
`helpers.js`, that the time grows by at most 4x the linear multiple of a
run at 1/8 (identity: 1/16) the size. The ratio is machine-independent;
the sizes sit well past the ~20k entries at which V8 stops left-trimming
a shifted array, where the array-backed queues grew 40-500x.

Every chunk or byte is index-checked, so the buffers cannot lose or
reorder data as they grow and wrap.

## Cells

`scaling-cpp.wd-test` and `scaling-ts.wd-test` share `scaling-modules.capnp`.
Both run with `timeout = "long"` (the C++ cell takes about 16 s, the TS
cell about 4 s), are tagged `no-asan`, and have no `@gc-stress` variant:
a GC per continuation would drown the timing. There is no legacy cell;
nothing here depends on a flag's unflagged side.

## Module map

| Module | Shapes |
| --- | --- |
| `readable.js` | pre-buffered backlog read back (20k → 160k); the same through both tee branches (10k → 80k); unawaited reads satisfied by enqueues (20k → 160k) |
| `readable-byte.js` | 64-byte BYOB fills over a backlog of one-byte chunks (16k → 128k); unawaited one-byte BYOB reads filled by one enqueue (10k → 80k) |
| `writable.js` | unawaited writes drained by a synchronous sink (20k → 160k) |
| `identity.js` | unawaited one-byte writes into an IdentityTransformStream, read back (5k → 80k) |

No divergences: both implementations are linear on every shape.
