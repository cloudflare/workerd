# Blob

An informal specification of Blob and File as implemented in workerd, kept
in lockstep with the test suite in this directory. **The tests are the
normative artifact.** The cells run the same modules (`blob-modules.capnp`):

| Cell | Implementation | Flags the suite branches on |
| --- | --- | --- |
| `blob-cpp` | `api/blob.{h,c++}` | `workers_api_getters_setters_on_prototype`, `set_tostring_tag` |
| `blob-legacy-flags-cpp` | the same | `workers_api_getters_setters_on_instance`, `do_not_set_tostring_tag` |
| `blob-opt-in-cpp` | the same, with the dateless opt-ins | as `blob-cpp`, plus `spec_compliant_property_attributes`, `immutable_api_prototypes`, `no_resizable_array_buffer_in_blob` |
| `blob-ts`, `blob-legacy-flags-ts`, `blob-opt-in-ts` | `src/per_isolate/blob/blob.ts`, under the `TS_BLOB` autogate | as the C++ cell of the same name |

`which-impl.js` reads the flags; a cell's `USING_TS_IMPL` binding says which
implementation it means to run. The C++ cells have no `@all-autogates`
variant, since enabling every autogate enables `TS_BLOB`.

What JavaScript cannot see is pinned in C++: the bytes a Blob serializes to,
what reading them creates, and what `JsBlob` and `JsFile`
(`api/blob-bridge.h`) make and read, in `api/blob-bootstrap-test.c++`; its
`@all-autogates` variant runs them against the TypeScript implementation.

## Core semantics

- **Construction:** `new Blob(parts, options)`. `parts` is undefined or a
  real Array (not any iterable), each element read in turn: a Blob or a
  BufferSource is kept, anything else (a SharedArrayBuffer included) is
  converted to a string. Then `options` (undefined, null or an object):
  `type`, then `endings`, which must be undefined. A proxy, even over an
  array, is not an Array. `new File(parts, name,
  options)` converts `name` between them, and reads `lastModified` (NaN as
  0, absent as now) between `type` and `endings`.
- **Type:** discarded if any code unit is outside 0x20 to 0x7F, otherwise
  lower-cased.
- **Checks:** once every part is converted, each in turn is checked: under
  `no_resizable_array_buffer_in_blob`, a BufferSource over a resizable
  ArrayBuffer or growable SharedArrayBuffer throws `TypeError`; then the
  running total against the isolate's size limit, `RangeError`. A string
  part is encoded as it is checked; the parts are combined only once all
  pass.
- **Strings:** a string part, and a File's name, become bytes as JSG makes a
  `kj::String`: WTF-8, so a lone surrogate is written as its own three bytes.
  `name` reads those bytes decoded, each such surrogate as three U+FFFDs. A
  type or name C++ gives as bytes that are not UTF-8 is kept as those bytes
  for C++ to read back (`api/blob-bootstrap-test.c++`).
- **`slice(start, end, type)`:** each converted as JSG converts an `int`
  (non-finite as 0, truncated, a `TypeError` outside int32), negative values
  counted from the end; always a new Blob, never a File.
- **Reading:** `arrayBuffer()` and `bytes()` copy; `text()` decodes UTF-8,
  replacing invalid sequences and keeping a byte order mark; `stream()`
  needs a request. A bad receiver throws synchronously.
- **Layout:** the attributes are prototype accessors, or under the legacy
  layout own read-only data properties of each instance, a File's before a
  Blob's.
- **Serialization:** a Blob as its type and bytes (tag 15), read back as a
  Blob of the installed implementation; a File throws `DataCloneError`.
- **Consumers:** `Body.blob()`, Blob bodies, `FormData` entries and parsing
  make and take either implementation through the bridge.

## Ledger

Where the TypeScript implementation differs from the C++. A row is pinned on
both sides: in JavaScript with a `usingTsImpl` branch, in C++ with
`JsBlob::isTsImplementation()`.

There are none: the suite runs the same assertions against both.
