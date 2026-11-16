# src/per_isolate/blob/

`Blob` and `File` in TypeScript, installed by `main.ts` under the `TS_BLOB`
autogate in place of the C++ classes (`api/blob.h`), which `global-scope.h`
then leaves out. Parent directory conventions apply; see
`src/per_isolate/AGENTS.md`.

## MODULES

| Module | Role |
| --- | --- |
| `blob.ts` | Both classes, every argument conversion, the shape; `isBlob()` for `url/url` |
| `cpp_exports.ts` | What the C++ bridge reads: the prototypes, and whether instances have own attributes |

`blob` requires `runtime-class`, `serialization` and `url/internal-array`, a
leaf. `url/url` requires `blob` under the autogate, for `createObjectURL()`.

## SEMANTICS

`src/tests/blob/AGENTS.md` describes the behavior and keeps the ledger of
every departure from the C++. Its suite runs against both implementations;
change the code and the ledger together.

## INVARIANTS

- **Native code makes and reads instances without running JavaScript**
  (`api/blob-bridge.{h,c++}`, `JsBlob` and `JsFile`), in V8's deserializer
  among other places. So an instance's state is under V8-private properties,
  not `#private` fields, and the bridge's `newTsInstance()` makes one exactly
  as `initInstance()` does: the same private keys (shared by name with
  `blob-bridge.c++`), the legacy layout's own properties in the same order,
  and the same serialization mark. Change both together.
- **The bytes are never written.** A Blob from a single Blob, a slice, and a
  File the bridge makes from a Blob share their parent's Uint8Array; the
  reading methods and the constructor copy.
- **Serialization:** a Blob is marked with `markStandIn()` as the C++ Blob's
  tag (15), so `Blob::serializeStandIn()` writes it in the C++ format and
  `Blob::deserialize()` reads it as whichever implementation is installed.
  A File is uncloneable, as in C++. Bytes are pinned in
  `api/blob-bootstrap-test.c++`.
- **No species or prototype lookups on the bytes:** views and copies are
  made with `new Uint8Array(...)` and the captured getters and `set()`, never
  `slice()` or `subarray()`, which look up a constructor user code can
  replace. Internal lists use `internalArray()`.
- **BufferSource parts are measured after every part is converted**, since
  converting a later part (its `toString()`) can resize an earlier one's
  buffer, as `api/tests/blob-test.js` checks.
