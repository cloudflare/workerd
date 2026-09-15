# Cache API × streams

The Cache API consuming and producing stream bodies under both stream
implementations. **The tests are the normative artifact.** The general
Cache API surface (headers, vary, purge, instrumentation) is owned by
`src/workerd/api/tests/cache-*`; this suite owns the STREAMS
interaction only.

## Infrastructure

All cells wire `cacheApiOutbound` to a loopback `cache-backend` worker
(cache-backend.js). A cache.put() arrives there as a PUT whose body is
the SERIALIZED HTTP RESPONSE (status line + headers + CRLFCRLF + body);
the backend splits at the header boundary, verifies the continuous
prime-modulus byte pattern over the body, extracts the serialized
head's Content-Length, and records everything. The test worker reads
the record back through its MOCK service binding (/last-put).

## Coverage (parity — no divergences observed)

| Test | Shape |
| --- | --- |
| `putJsValueStreamBody` | value stream of Uint8Array chunks, byte-exact at the backend |
| `putJsByteStreamBody` | 64 KiB chunked byte stream |
| `putFixedLengthStreamBody` | FixedLengthStream body; the declared length arrives as a concrete Content-Length in the serialized head |
| `putIdentityStreamBody` | identity body fed by a concurrent writer |
| `putLargeStreamBody` | 1 MiB chunked, byte-exact |
| `putDisturbedBodyRejects` / `putLockedBodyRejects` | TypeError before any backend traffic |
| `putErroringBodyRejects` | source error rejects the put |
| `concurrentClonePuts` | the migrated cache-put-stream-test.js regression: clone + concurrent puts over a live TransformStream body, 1 MiB |
| `matchBodyIsReadableStream` | a HIT body streams out and drains via a reader |

## Compatibility flags

The main C++ cell pins `streams_enable_constructors` and
`transformstream_enable_standard_constructor`. `cache-cpp-legacy` runs
`concurrentClonePuts` with only `nodejs_compat`, retaining coverage of the
original TransformStream alias. Its `@all-compat-flags` variant is disabled
because that variant would enable `streams_enable_constructors`, allowing the
modern path selected by `transformstream_enable_standard_constructor` and
duplicating the main C++ cell.

The backend worker takes no compatibilityDate — wd_test injects
`--compat-date`, and a worker-level date conflicts with it.
