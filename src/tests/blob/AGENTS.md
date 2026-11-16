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
  running total against the isolate's size limit, `RangeError`. Nothing is
  copied until all pass.
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

Where the TypeScript implementation differs from the C++, toward WebIDL.
Each row is pinned on both sides: in JavaScript with a `usingTsImpl` branch, in C++ with
`JsBlob::isTsImplementation()`.

| # | Area | C++ | TypeScript | Test |
| --- | --- | --- | --- | --- |
| 1 | Calling without `new` | JSG's `TypeError` message | the language's | `construction.js` |
| 2 | `File`'s `[[Prototype]]` | `Function.prototype` | `Blob`, as WebIDL says (so WPT's `idlharness.any.js` passes "File interface: existence and properties of interface object") | `api-surface.js` |
| 3 | Constructors' `prototype` | writable | not writable | `api-surface.js` |
| 4 | Lone surrogates in string parts | their WTF-8 bytes | U+FFFD's, as for a USVString (so WPT's `unicode.any.js` passes) | `construction.js` |
| 5 | Lone surrogates in a File's name | kept as WTF-8 bytes, so `name` reads each as three U+FFFDs and a `FormData` body writes the bytes | U+FFFD, as for a USVString, in `name` and the body alike | `construction.js` |
| 6 | A type that is not UTF-8, as a Blob made from a response's `Content-Type` can have | kept as bytes, which C++ that reads the type sees (a Blob body's `Content-Type`, for one) | kept as the string JavaScript sees of either, the bytes decoded with U+FFFD for each invalid sequence | `api/blob-bootstrap-test.c++` |
