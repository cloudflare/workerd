# Workers Wasm ABI: conventions

This document specifies how a WebAssembly module ("the guest") calls the Workers API ("the
host"). The imports are grouped into Wasm import modules such as `cf:core/v1` and `cf:http/v1`.
Most of them are the existing Workers API, lowered from JSG method signatures by the fixed rules
below; the others are defined by this document.

The **shim** is a set of JavaScript modules that implements every import on today's JavaScript
API: a shared runtime (`shim/runtime.js`) and one generated shim module per import module
(`shim/cf-core-v1.js`, `shim/cf-http-v1.js`). It is the reference implementation. Any other
implementation, such as a native host that implements the imports in C++ against the JSG
objects, must be observably equivalent to it: same results, same errors, same handle numbers,
same sequence of host-side effects. This document is normative; where it and the shim disagree,
one of them has a bug, and the known cases are listed under [Known deviations](#known-deviations).

What "same errors" covers: the class (`name`) of every thrown value, and the message of errors
this document defines (the conventions-defined imports and `respond`). Messages of errors the API
throws come from the same JSG code in every implementation and match as a consequence. Messages
V8 generates for JS-level misuse (for example `x is not a function`) and trap messages are not
normative.

Contents: [Overview](#overview) · [Modules](#modules) · [Guest module](#guest-module) ·
[Handles](#handles) · [Status codes, errors and traps](#status-codes-errors-and-traps) ·
[Lowering rules](#lowering-rules) · [Result slots](#result-slots) ·
[Import names](#import-names) · [Suspension](#suspension) · [Callbacks](#callbacks) ·
[respond](#respond) · [Compatibility dates](#compatibility-dates) ·
[Pre-initialization](#pre-initialization) · [`cf:core/v1`](#cfcorev1) ·
[Worked example: `Headers.get`](#worked-example-headersget) ·
[Worked example: `fetch`](#worked-example-fetch) · [Tooling](#tooling) ·
[Planned modules](#planned-modules) · [Known deviations](#known-deviations) ·
[Open questions](#open-questions)

## Overview

- Imports live in **Wasm import modules**, each named and versioned on its own and append-only
  (see [Modules](#modules)). This version has two: `cf:core/v1`, the handle table and the
  imports this document defines, and `cf:http/v1`, the fetch API. Every module depends on
  `cf:core/v1`. The first versions are deliberately minimal: `cf:http/v1` has what a guest needs
  to handle a request and make subrequests, and grows by appending imports
  (see [Growth](#growth)).
- **Lowered imports** correspond one-to-one to JSG members (methods, static methods,
  constructors, property getters and setters) and are generated from JSG RTTI
  (`src/workerd/jsg/rtti.capnp`).
- **Conventions-defined imports** (handle table, generic values, objects, arrays, errors,
  bindings, `respond`, console, panic) are fixed by this document; see [`cf:core/v1`](#cfcorev1).
- Every import can be implemented natively without running JavaScript per request: in C++
  through JSG and V8 API calls, which may construct and populate V8 objects (dictionaries,
  arrays, strings, errors) but never call into script.
- Host objects never enter guest memory. The guest refers to them by **handle**, an `i32` index
  into a per-request table, and releases them explicitly.
- Imports that return a promise **suspend** the guest (JSPI) until the promise settles. From the
  guest's view, every import is a blocking call.
- Every request runs in a **fresh instance** of the module. Nothing survives between requests
  except what `init` produced (see [Pre-initialization](#pre-initialization)).
- Each module's **record**, `modules/<stem>.json` (for example `modules/cf-http-v1.json`), is the
  machine-readable form of everything below: for each import its name, Wasm signature, lowered
  parameter and result types, result slot layout, the JSG member it came from and its
  compatibility conditions.

## Modules

### Names

A module is named `cf:<name>/v<version>`: `<name>` is a lower-case letter followed by lower-case
letters and digits, `<version>` a positive decimal integer without leading zeros. Its record is
`modules/cf-<name>-v<version>.json` and its shim module `shim/cf-<name>-v<version>.js`.

Wasm import module names are arbitrary UTF-8 strings, so this format is a convention. It is
written as a string in every toolchain that sets import modules, for example
`#[link(wasm_import_module = "cf:http/v1")]` on an `extern` block in Rust, or
`__attribute__((import_module("cf:http/v1"), import_name("Headers#get")))` in C and C++ with
Clang. The `cf:` prefix keeps the modules apart from those of other conventions (`env`,
`wasi_snapshot_preview1`); the version is part of the name, so the imports of a guest say which
version of each module it was built against; and the name maps to a file name by replacing `:`
and `/` with `-`. The form resembles the package names of the WebAssembly component model, but
these are plain import module names of core Wasm, not WIT interfaces.

| Module | Imports |
|---|---|
| `cf:core/v1` | The conventions-defined imports: handle table, values, objects, arrays, errors, `env_get`, `respond`, `console_log`, `panic`. |
| `cf:http/v1` | Lowered members of `Headers`, `Body`, `Request` and `Response`, and the global `fetch` (see [Growth](#growth)). |

### Dependency on `cf:core/v1`

Every module depends on `cf:core/v1`. There is one handle table per request, which `cf:core/v1`
owns; a handle that one module returns is valid in every other, and `handle_drop`, the generic
values and the error accessors work on all of them. An implementation that provides any module
also provides `cf:core/v1`.

### The modules a guest requires

A guest requires exactly the import modules that its imports name, as
`WebAssembly.Module.imports` lists them. There is no other declaration. A guest that imports only
from `cf:core/v1` and `cf:http/v1` runs wherever both are implemented.

### Versioning

A module version is **released** once its record is checked in. Its imports are permanent: an
import, once released, keeps its name, Wasm signature and meaning forever, also if its JSG member
is deprecated later. A module only grows, by new imports at the end of its record: a member added
to the include list of a class of `cf:http/v1` becomes a new import of `cf:http/v1` (see
[Growth](#growth)).

The guest depends on an import's Wasm signature, on whether it suspends, and on the shape of its
lowered types: their kinds, the number flags (`unsigned`, `bigint`, `date`), and the variants of
a union, their order and, in results, the classes they tell apart. These must not change. What
only the API sees may change: the JSG string kind, whether an absent optional is passed as
`null` or `undefined`, and which C++ type a handle refers to.

A change that would break a released import needs a new version of its module (`cf:http/v2`), which
guests opt into by importing it, while the old version stays. New APIs come as new modules (see
[Planned modules](#planned-modules)). The generator enforces this against the records: it keeps
every released import in its place and appends new ones, and it refuses to generate a module, naming
the import, if a released import was taken out of its include list, its member has disappeared from
the RTTI, or its signature changed in a way the guest would notice. How an old version keeps being
served once its members have changed is decided when the first such change is needed.

### Growth

A module contains the members of its classes that its **include list** names, in `MODULES` in
`generator/generate.py`, plus its conventions-defined imports. The other members are not added
yet; nothing about them is decided. To add a member, append its import name (see
[Import names](#import-names)) to its class's list and regenerate: it becomes a new import at the
end of the module, and the self-test checks that it exists and is current. An include list is
append-only like the module itself: taking out a released import fails.

Version 1 of `cf:http/v1` is deliberately minimal:

| Class | Imports |
|---|---|
| `Headers` | `Headers.constructor`, `#get`, `#has`, `#set`, `#append`, `#delete`, `#getSetCookie`, `#forEach` |
| `Body` | `Body#text`, `#bytes` |
| `Request` | `Request.constructor`, `#method.get`, `#url.get`, `#headers.get` |
| `Response` | `Response.constructor`, `#status.get`, `#statusText.get`, `#headers.get` |
| global | `globalThis.fetch` |

Bodies are read and written whole, as strings or bytes; `Body#body.get` comes with
[`cf:streams/v1`](#planned-modules).

### Deprecated members

No module contains deprecated functionality when it is released. A member that is already
deprecated when it would first be added is never added: it is listed, with the reason, in
`DEPRECATED` in `generator/generate.py`, and the record lists it under `excluded`. A member is
deprecated if

- the JSG or C++ code says so in a comment, or the TypeScript declarations mark it `@deprecated`;
- a compatibility flag removes or replaces it at the newest compatibility date; or
- it is a legacy non-standard extension documented as superseded.

The generator refuses to add an included member that is listed in `DEPRECATED`, or that every
TypeScript declaration of it, or of its class, marks `@deprecated`
(`types/generated-snapshot/index.d.ts`, checked automatically). An import that was released
before its member became deprecated stays, because released imports are permanent; its record
entry gets a `deprecated` field with the reason, and the self-test fails until the regenerated
record with that marking is checked in. Guests should not use deprecated imports, and
implementations still provide them.

Excluded from `cf:http/v1`: `Headers#getAll`, a legacy non-standard extension that `getSetCookie`
supersedes.

### Launcher

A Wasm-only worker consists of the launcher (`launcher.js`) as its main module, the shim runtime
(`runtime.js`), the shim module of every import module its guest requires
(`cf-<name>-v<version>.js`), and the guest (`worker.wasm`), all at the top level of the bundle.
A bundle carries only the shim modules its guest uses. The launcher is the same for every guest
and every set of modules:

1. It imports the built-in module `workerd:wasm-abi-native`. If the runtime provides it, and
   its default export's `serve(module)` returns a handler, that handler serves the guest. A
   native host returns `undefined` to decline, for example when it does not implement every
   module the guest requires; the shim then serves the guest. No runtime provides the module
   yet.
2. Otherwise it collects the distinct import module names of `worker.wasm`. A name that is not a
   module name as above fails with
   `TypeError: Wasm import module <name> is not a module of the Workers Wasm ABI`.
3. It loads `./cf-<name>-v<version>.js` for each of them with `import()`. A shim module that the
   bundle lacks, or that fails to load, fails with `Error: cannot load <file>, the shim module of
   Wasm import module <name>`, whose `cause` is the error of `import()`.
4. It exports `serve(wasm, shims)` of `runtime.js` as the worker's default export, where `shims`
   are the default exports of the shim modules.

Workerd resolves these `import()` calls, whose specifiers are computed at run time, against the
bundle's own modules with both module registries; `tests/launcher-test.wd-test` covers both.

### Recognition by embedders

An embedder that wants to serve Wasm-only workers natively can recognize one from its bundle
alone, without running it:

- its main module is the launcher, with bytes the embedder knows;
- it has a module `runtime.js` whose hash is that of a known version of the shim runtime;
- each of its other JavaScript modules is named after a module (`cf-<name>-v<version>.js`) and
  has the hash of a known version of that module's shim module;
- it has exactly one Wasm module, `worker.wasm`, and every import module of it has a shim module
  in the bundle.

Any other bundle is an ordinary worker. Regenerating a shim module, for example after an import
was appended, changes its hash, so an embedder keeps the hashes of every version it accepts.

## Guest module

### Exports

| Export | Signature | Required | Meaning |
|---|---|---|---|
| `memory` | memory | yes | The guest's linear memory. Must declare a maximum. |
| `handle` | `(request: i32, env: i32, ctx: i32) -> i32` | yes | The fetch handler. |
| `init` | `() -> ()` | no | Pre-initialization; runs once per module version, before any request. |
| `cf_callback` | `(token: i32, args: i32) -> i32` | if the guest passes callbacks | Callback dispatcher. |

`handle` receives handles to the `Request`, the `env` object and the execution context, which are
always handles 1, 2 and 3 (allocated in that order, into an empty table). The guest
owns them. It returns:

- `h > 0`: a handle to the `Response`. The host takes ownership of `h`.
- `h < 0`: the request fails by throwing the value of handle `-h` (taken by the host).
- `0`: only valid after [`respond`](#respond); otherwise the request fails with
  `Error("handle returned no response")`.

The host calls `handle` through `WebAssembly.promising` (in the shim; a native host uses an
equivalent mechanism), so `handle` may block in suspending imports.

### Memory

- Exactly one memory, 32-bit, exported as `memory`, with a **declared maximum** (with LLVM, the
  linker flag `--max-memory`). Its default is an [open question](#open-questions).
- Guest pointers passed to an import are byte offsets into `memory`. The host checks guest ranges
  (`ptr + len <= memory size`, with `ptr` and `len` read as unsigned) **before the API is
  called**, in this order: the parameters left to right (a `string` or `bytes` parameter's
  `[ptr, ptr + len)`; nothing for an absent `maybe`), then the whole result slot, then, if the
  result slot contains a StrSlot, its buffer `[ptr, ptr + cap)`. An out-of-range pointer is a
  [trap](#status-codes-errors-and-traps) and the API is not called.
- Result slots need not be aligned; the alignments below are recommended.
- A pointer is valid only during the import call that receives it, including while that call is
  suspended. The host never keeps a guest pointer after the call returns.
- Memory growth is the guest's business; the host re-reads the memory size on every access.

### Wasm feature profile

Guests should stay within Wasm 1.0 plus bulk memory, sign extension, non-trapping float-to-int,
multi-value and mutable globals. None of the imports need reference types, exceptions, GC, SIMD,
threads or memory64. The shim does not enforce the profile.

## Handles

The handle table belongs to one request and is shared by all modules. A **handle** is an `i32`
that names one table entry; the entry holds one JS value (any value, including `undefined` and
`null`).

- Handle `0` is the **null handle**. It never names an entry. Where a parameter or result is an
  optional handle, `0` means absent.
- Valid handles are `1, 2, 3, ...`. Allocation reuses the most recently released handle first
  (a LIFO free list); if none is free, it takes the next number never used. This rule is part of
  the ABI because handle numbers appear in traces and must match between implementations.
- `handle_drop(h)` releases an entry. Releasing a handle that is not live is a trap.
- At the end of a request (the handler returned and every pending operation settled, or the
  request was abandoned) the whole table is discarded.

Ownership:

- **Results are owned.** A handle the host returns (directly or in a result slot) is a new entry
  owned by the guest, even if it names an object the guest already holds under another handle:
  calling `Request#headers.get` twice yields two different handles to the same `Headers`.
- **Parameters are borrowed.** Passing a handle to an import does not transfer it; the guest still
  owns it and must drop it. (Exception: the return value of `handle` and of `cf_callback`.)
- Error handles returned as status codes are owned, like any result.

## Status codes, errors and traps

Every lowered import, and every conventions-defined import marked `rc` in the table, returns an
`i32` **status**:

- `0`: success. A non-void result has been written to the result slot (the last parameter).
- `s > 0`: the operation threw; `s` is a new handle holding the thrown value (usually an
  `Error`). Nothing has been written to the result slot. Read it with `error_name` and
  `error_message`, pass it on, or drop it.

What counts as "threw" is exactly what throws (or rejects) in JavaScript: a `TypeError` for a bad
argument, a `DOMException`, a rejected promise from `fetch`, and so on.

**Traps** are guest bugs, not API errors. They are not reported as status codes; they unwind the
guest and fail the request as an uncaught exception whose `name` is `GuestFault` (the client sees
what any uncaught exception produces, a 500 for a fetch). The host performs no further effects
for the guest after a trap. Traps are:

- an invalid handle (0 where a value is required, never allocated, or already dropped);
- a guest range outside memory;
- an invalid union tag;
- calling an import other than `panic` during `init`;
- calling a suspending import inside a callback (the host's `SuspendError`), or any trap inside a
  callback;
- `panic(msg)`;
- `array_len` on a value that is not an array.

## Lowering rules

The generator lowers each JSG member's RTTI to Wasm in two steps: it maps each RTTI type to a
**lowered type** (L-type), then maps the L-type to Wasm parameters or to a result slot.

### RTTI type to L-type

| RTTI kind (`rtti.capnp` `Type`) | Parameter L-type | Result L-type |
|---|---|---|
| `voidt` | (none) | `void` |
| `boolt` | `bool` | `bool` |
| `number` | `i32`, `i64` or `f64` (see below) | same |
| `string` | `string(kind)` | same |
| `structure` (JSG resource or JSG struct) | `handle` | `handle` |
| `object`, `unknown`, `dict`, `intrinsic`, `jsBuiltin` | `handle` | `handle` |
| `array` of `unsigned char`, `char` or `signed char` | `bytes(ArrayBuffer)` | `bytes` |
| `array` (any other element) | `handle` (a JS array) | `handle` |
| `builtin` `v8Uint8Array`, `v8ArrayBufferView`, `jsgBufferSource` | `bytes(Uint8Array)` | `bytes` |
| `builtin` `v8ArrayBuffer` | `bytes(ArrayBuffer)` | `bytes` |
| `builtin` `kjDate` | `f64(date)`: ms since the epoch | `f64(date)` |
| `builtin` (other typed arrays, `v8Function`, `v8Symbol`, `v8TypedArray`, `jsgError`) | `handle` | `handle` |
| `function` | `callback` | `handle` (see [Open questions](#open-questions)) |
| `promise` | `handle` (a JS promise) | makes the import **suspending**; the result is the L-type of the awaited value |
| `maybe` | `maybe(inner, absent)` | `maybe(inner)` |
| `oneOf` | `oneof(variants)`, or `handle` if every variant is a handle | `oneof(variants)` |
| `jsgImpl` `jsgVarargs` | `varargs`: a handle to a JS array, spread into the call | n/a |
| `jsgImpl` `jsgName` | `handle` | `handle` |
| `jsgImpl` (all others: `configuration`, `v8Isolate`, `jsgLock`, `jsgTypeHandler`, `jsgSelfRef`, `v8FunctionCallbackInfo`, `v8PropertyCallbackInfo`, `jsgUnimplemented`) | omitted (JS never passes these) | n/a |

Number types (`FOR_EACH_NUMBER_TYPE` in `jsg/rtti.h`):

| C++ type | L-type | Wasm | JS value |
|---|---|---|---|
| `char`, `signed char`, `short`, `int`, `jsg::JsInt32` | `i32` | `i32` | Number |
| `unsigned char`, `unsigned short`, `unsigned int`, `jsg::JsUint32` | `i32` unsigned | `i32` (read as unsigned) | Number |
| `long`, `unsigned long`, `long long`, `unsigned long long` | `i64` | `i64` | Number (converted with `Number()`, so values beyond 2^53 lose precision) |
| `jsg::JsBigInt` | `i64` bigint | `i64` | BigInt |
| `double`, `jsg::JsNumber` | `f64` | `f64` | Number |

Normalization, applied while lowering, in this order:

1. A result `oneOf` whose variants are all promises is replaced by a `oneOf` of the promised
   types, and the import becomes suspending. (A single remaining variant stands alone.)
2. `maybe(maybe(T))` (for example `jsg::Optional<kj::Maybe<T>>`) is one `maybe(T)`; the outer
   type decides `absent`.
3. The variants of a `oneOf` are lowered left to right, and each contributes a list of parts: a
   `oneof` variant contributes its own variants (flattening); a `maybe` variant contributes its
   inner type, or the inner `oneof`'s variants, and marks the union optional (hoisting; the first
   such `maybe` decides `absent`); any other variant contributes itself. Parts are appended in
   order, skipping any whose L-type equals one already present (merging; the first occurrence
   keeps its position). Tags are the positions in the resulting list.
4. For a parameter, a union whose parts are all `handle` becomes a single `handle`: JSG tells JS
   objects apart itself, so the guest passes the object and no tag.
5. A union marked optional in step 3 is wrapped as `maybe(oneof(...))` (or `maybe(handle)` after
   step 4).

Example, the body parameter of `Response.constructor`,
`jsg::Optional<kj::Maybe<kj::OneOf<ReadableStream, kj::String, jsg::BufferSource, Blob, FormData,
URLSearchParams, url::URLSearchParams, jsg::AsyncGeneratorIgnoringStrings<...>>>>`: step 2 makes
it one `maybe` whose `absent` is `undefined` (from `jsg::Optional`); in step 3 the variants lower
to `handle(ReadableStream)`, `string`, `bytes(Uint8Array)`, `handle(Blob)`, `handle(FormData)`,
`handle(URLSearchParams)` (both URLSearchParams classes lower alike, so the second is merged) and
`handle(Array)`, with tags 0 to 6; step 4 does not apply, because `string` and `bytes` are not
handles. The parameter is `present, tag, p0, p1`, all `i32`: a string body is `(1, 1, ptr, len)`,
a stream `(1, 0, handle, 0)`, no body `(0, 0, 0, 0)`.

`absent` of a `maybe` is `null` for `kj::Maybe` and `undefined` for `jsg::Optional` and
`jsg::LenientOptional` (from the RTTI's `allowsNull`/`allowsUndefined` flags, or the type name
when neither flag is set). It is what the host passes to the API when the guest says "absent".
The host always passes every parameter: an absent optional is an explicit `undefined` (or
`null`) argument, never a shorter argument list.

### L-type to Wasm parameters

| L-type | Wasm parameters |
|---|---|
| `bool` | `i32`: 0 is false, anything else true |
| `i32`, `i64`, `f64` | one parameter of that type |
| `string`, `bytes` | `ptr: i32, len: i32` (a guest byte range) |
| `handle`, `varargs` | `i32` handle |
| `callback` | `i32` callback token (see [Callbacks](#callbacks)) |
| `maybe(T)`, `T` a `handle` or `callback` | `i32`; `0` means absent |
| `maybe(T)`, otherwise | `present: i32`, then `T`'s parameters (ignored when `present` is 0, but always passed) |
| `oneof(V0, V1, ...)` | `tag: i32`, then the payload slots |

Instance methods and property accessors take the receiver (`this`) as a leading `i32` handle.
Imports with a non-void result take a trailing `ret: i32` pointer to the result slot. All lowered
imports return the `i32` status.

**Union payload.** Variant `tag` (0-based, in the normalized order) lowers to its own parameter
list. The payload has as many slots as the longest variant list. Slot *k* is `i32` if every
variant's *k*-th parameter is `i32` (or the variant has fewer parameters); otherwise it is `i64`.
A variant's parameters are placed left-aligned: an `i32` in an `i64` slot is zero-extended, an
`f64` travels as its IEEE-754 bit pattern, and unused slots should be 0; the host ignores them,
and it does not look at the tag or payload at all when an enclosing `maybe` is absent. For example
`oneof(handle(Request), string)` is `tag, p0: i32, p1: i32`: a Request is `(0, handle, 0)` and a
URL is `(1, ptr, len)`.

### Strings

Guest to host (parameters): the bytes are decoded as UTF-8 with the WHATWG decoder in replacement
mode (`TextDecoder('utf-8', {fatal: false, ignoreBOM: true})`): each maximal invalid subsequence
becomes one U+FFFD, and a leading byte-order mark is kept as U+FEFF. The resulting JS string is
passed to the API, which converts it according to its JSG string kind exactly as it would a JS
caller's string:

| JSG kind (RTTI `StringType.name`) | What the API sees |
|---|---|
| `kj::String`, `kj::StringPtr` | the string's UTF-8 encoding (lone surrogates cannot occur after decoding) |
| `USVString` | the string with lone surrogates replaced (none occur after decoding) |
| `DOMString` | the string |
| `v8::String`, `jsg::JsString` | the JS string |

So a native implementation may pass valid UTF-8 straight through to `kj::String` parameters and
must replace invalid sequences exactly like the WHATWG decoder.

Host to guest (results): the JS string is encoded as UTF-8 with the WHATWG encoder, which replaces
each lone surrogate with U+FFFD. The guest always receives valid UTF-8.

### Results by L-type

The JS value an API returns is converted to the result slot as follows:

| L-type | Conversion |
|---|---|
| `bool` | truthiness, written as 0 or 1 |
| `i32` | `Number(v) | 0` (the same 32 bits for unsigned) |
| `i64` | `BigInt(v)` wrapped to 64 bits |
| `f64` | `Number(v)`; a `date` writes `v.getTime()` |
| `string` | UTF-8 of `String(v)` |
| `bytes` | the bytes of an `ArrayBuffer` or view |
| `handle` | a new handle to `v`, whatever `v` is |
| `maybe(handle)` | 0 if `v` is `null` or `undefined`, else a new handle |
| `maybe(T)` | `present = 0` if `v` is `null` or `undefined`, else `present = 1` and `T` |
| `oneof(...)` | `tag` = the first variant that accepts `v` (below), then that variant |

A union result variant accepts a value as follows: `bool` a boolean; `i32`/`f64` a number
(`f64(date)` a `Date`); `i64` a number or BigInt; `string` a string; `bytes` an `ArrayBuffer` or
view; `handle` of a global JSG class (`ReadableStream`, `Response`, ...) an `instanceof` that
class; `handle(Array)` an array; `handle(unknown)` anything; any other `handle` any object or
function. The first accepting variant wins, in the normalized order, even if the C++ code
produced a later variant (a native implementation must apply the same test to the value it would
have returned to JS). If no variant accepts, the import fails with a `TypeError`.

The tag therefore reports the JS type of the value, not which C++ alternative produced it: a
union of `kj::String` and `jsg::JsValue` reports a JS string as `string` even where the C++ code
produced the `jsg::JsValue`. A guest must know what it asked for.

## Result slots

A result slot is guest memory, passed as the trailing `ret` pointer, that the host writes on
success. Layouts (little-endian, offsets in bytes):

| L-type | Size | Align | Layout |
|---|---|---|---|
| `bool`, `i32`, `handle`, `callback` | 4 | 4 | `u32` value |
| `i64`, `f64` | 8 | 8 | the value |
| `string`, `bytes` | 16 | 4 | **StrSlot**: `ptr: u32` (in), `cap: u32` (in), `len: u32` (out), `overflow: u32` (out) |
| `maybe(handle)` | 4 | 4 | handle, 0 if absent |
| `maybe(T)` | 8 + T rounded up to 8 | 8 | `present: u32` at 0, `T`'s slot at 8 |
| `oneof(V...)` | 8 + max(V) rounded up to 8 | 8 | `tag: u32` at 0, the variant's slot at 8 |

**StrSlot protocol.** Before the call the guest stores a buffer pointer and capacity in `ptr` and
`cap`. Inside a `maybe` or `oneof` slot, the StrSlot sits where the inner value goes (offset 8 of
each enclosing `maybe` or `oneof`); every string or bytes variant of a union shares that offset,
so the guest fills it once and the host finds it whichever variant it writes. The host writes
the byte length to `len`. If `len <= cap` it copies the bytes to `ptr` and sets `overflow = 0`.
Otherwise it writes nothing to `ptr` and sets `overflow` to a new handle holding a fresh
`Uint8Array` copy of the bytes (never a view of the API's buffer); the guest reads them with
`value_to_bytes` into a buffer of `len` bytes and drops the handle.

The host writes only the fields listed for the value it stores: `present` (and the inner slot
only when present), `tag` and the chosen variant's slot, a StrSlot's `len` and `overflow`. Every
other byte of the slot (padding, other variants' bytes, `ptr` and `cap`, the inner slot of an
absent `maybe`) is left as the guest wrote it. Nothing at all is written when the status is
not 0.

## Import names

An import is identified by its module and its name within the module.

| JSG member | Import name | Binding name (`binding` in the record) |
|---|---|---|
| instance method `m` of class `C` | `C#m` | `c_m` |
| static method | `C.m` | `c_static_m` |
| constructor | `C.constructor` | `c_new` |
| property getter / setter | `C#p.get` / `C#p.set` (setter only if not read-only) | `c_get_p` / `c_set_p` |
| global function (`ServiceWorkerGlobalScope`) | `globalThis.m` | `global_m` |
| conventions-defined | `snake_case`, no `#` or `.` | same |

`C` is the JSG class name (the RTTI `Structure.name`). Binding names and parameter names (in the
record) are not part of the ABI; they are what generated language bindings call the imports and
their parameters. They come from the RTTI's `argNames` when present, else from the TypeScript
declarations in `types/generated-snapshot/index.d.ts` with the same member name and arity, else
`arg0`, `arg1`.

Members inherited from a base class are imported once, on the base: `Request` and `Response`
share `Body#text` and `Body#bytes`, which accept either.

The receiver of an instance method or property import must be an instance of `C` (or a
subclass); otherwise the import fails with JSG's `TypeError: Illegal invocation`. The shim gets
this check from JSG by calling the member found on the global class: `C.prototype.m.call(self,
...args)` for methods, and the accessor of `C.prototype` for properties that JSG defines on the
prototype (`"proto": true` in the record; other properties are read as `self.p`). A class is
global if `globalThis[C]` is a constructor at run time; every class of `cf:http/v1` is. Static
methods and constructors are called on `globalThis[C]`, global functions on `globalThis`.

## Suspension

An import is **suspending** if its JSG member returns a promise (directly, or as a union of
promises), plus the conventions-defined imports marked suspending (none in `cf:core/v1`). The
host awaits the promise and only then writes the result and returns the status. While it waits,
the guest is parked: no guest code runs in this instance, and guest buffers passed to the call
stay valid, so the host may write into them directly.

A suspending import **always suspends exactly once**, even if the API throws synchronously or the
promise is already settled: the guest resumes in a later microtask, after the promise reaction
runs. (Traps are raised synchronously, without suspending.) The shim gets this from wrapping
suspending imports in `WebAssembly.Suspending` and always returning a promise from them; it calls
`handle` through `WebAssembly.promising`. A rejected promise becomes a status (an error handle),
not a trap.

Abandoned requests: when the client disconnects or the request is cancelled, the instance and its
handle table are discarded. A pending suspension never resumes.

## Callbacks

A parameter of JSG `function` type (for example the callback of `Headers#forEach`) lowers to a
**callback token**: a guest-chosen non-zero `u32`. The host wraps it in a JS function; when the
API calls that function with arguments `a0, a1, ...`, the host:

1. creates a handle to the array `[a0, a1, ...]`;
2. calls the guest export `cf_callback(token, args)`;
3. releases the argument array handle after `cf_callback` returns (the guest borrows it);
4. interprets the return value `r`: `0` returns `undefined` to the API; `r > 0` returns the value
   of handle `r` (taken by the host); `r < 0` throws the value of handle `-r` (taken).

Rules:

- Callbacks **may not block**. `cf_callback` is not wrapped in `promising`, so a suspending import
  inside a callback traps, failing the request.
- The host keeps no record of tokens. The guest keeps whatever state a token refers to until it
  knows the host will not call it again (after a synchronous API such as `Headers.forEach`
  returns, or at the end of the request). The shim does not notify the guest when a host object
  holding a callback is collected (see [Open questions](#open-questions)).
- A callback runs synchronously inside the import that triggered it, so it may call other
  non-suspending imports.

## respond

`respond(response)` sends the response immediately; the handler keeps running, for example to
make further subrequests, and finally returns `0`. The host keeps the request alive until the
handler returns (with `ctx.waitUntil` in the shim). An error the handler returns after
responding is logged with `console.error`. Calling `respond` twice fails with a `TypeError`.

## Compatibility dates

The imports of a lowered module depend on the worker's compatibility date and flags, because the
JSG members do. RTTI is exported for one date and flag set (`workerd:rtti`
`exportTypes(date, flags)`), and the generator produces the modules from one RTTI.

- The records and shim modules are generated at one compatibility date, the newest one
  (`compatDate` in the record of a lowered module, and the comment at the top of its shim
  module). `cf:core/v1` does not depend on the date.
- `generator/compat-gates.json` records, for each import whose presence differs between an older
  date and the newest one, the flag that enables it (found by `generator/compat_gates.py`, which
  dumps RTTI at the older date plus each candidate flag). The record carries it as
  `compat.enableFlag` and `compat.enableDate`.
- At run time the shim leaves an import out of the import object if its JSG member does not exist
  in the isolate (checked on the global class or its prototype). A guest that imports a member its
  compatibility settings do not enable fails to instantiate with a `LinkError` naming the import.
  A property that JSG defines on each instance (at dates without
  `jsg_property_on_prototype_template`) cannot be checked without an instance, so its imports are
  always present.
- Flags that change behavior without adding or removing members do not change the imports; the
  imports simply behave as the API does at the worker's settings.
- A flag that changes a member's *signature* would change an import's Wasm signature, which the
  append-only rule forbids. The self-test fails in that case.

## Pre-initialization

`init` is the module's pre-initialization entry point. It runs **exactly once per module
version**, and every request starts from the state it left: the **image**. The shim's image is the
reference semantics for any implementation that pre-initializes memory images:

1. The first time a module serves a request, the shim instantiates it in a scratch instance,
   records its memory and the values of its exported globals, calls `init`, and records them
   again. The image is the difference: the memory size after `init` (in 64 KiB pages), every
   4 KiB chunk of memory that differs (memory past the original size counts as differing where
   it is not zero), and every exported global whose value differs.
2. Each request instantiates the module fresh (so data segments are applied as usual), grows the
   memory to the image's size, copies the differing chunks in, sets the differing exported
   globals, and only then allocates handles 1 to 3 and calls `handle`.
3. If `init` traps or throws, the module has no image: every request to it fails with that same
   error, and `init` is not retried.

A module without `init` has an empty image: requests start from the freshly instantiated state.
For the image to capture everything `init` did, `init` must be:

- **deterministic**: the same image results every time it is built (an implementation may build
  the image in another process, on another machine, at upload time);
- **import-free**: it may call no import except `panic` (others trap), so it cannot observe a
  request, the environment, the time or randomness;
- **handle-free**: it ends with an empty handle table (which follows from the above);
- **confined to memory and exported globals**: non-exported mutable globals, tables and any other
  instance state must be back at their initial values when `init` returns. Toolchains satisfy
  this naturally: LLVM's stack pointer is restored on return, and Rust and C keep heap state in
  linear memory.

## `cf:core/v1`

The conventions-defined imports, all in `cf:core/v1`. `rc` returns a status and writes a non-void
result to the trailing slot; `direct` returns the result itself and cannot fail; `none` returns
nothing. Handle parameters are borrowed.

| Import | Parameters | Result | Return | Notes |
|---|---|---|---|---|
| `handle_drop` | `h` | | none | Releases a handle; traps if not live. |
| `value_undefined` | | handle | direct | `undefined` |
| `value_null` | | handle | direct | `null` |
| `value_bool` | `v: bool` | handle | direct | |
| `value_f64` | `v: f64` | handle | direct | |
| `value_string` | `s: string` | handle | direct | Decoded as in [Strings](#strings). |
| `value_bytes` | `b: bytes` | handle | direct | A new `Uint8Array` copy. |
| `value_kind` | `h` | u32 | direct | See the value kinds below. |
| `value_to_bool` | `h` | bool | direct | ToBoolean. |
| `value_to_f64` | `h` | f64 | rc | ToNumber; throws for a Symbol. |
| `value_to_string` | `h` | string | rc | ToString; throws for a Symbol. |
| `value_to_bytes` | `h` | bytes | rc | ArrayBuffer or view; else `TypeError`. |
| `object_new` | | handle | direct | `{}` |
| `object_set` | `obj, key: string, v` | | rc | `obj[key] = v` |
| `object_set_string` | `obj, key: string, v: string` | | rc | |
| `object_set_f64` | `obj, key: string, v: f64` | | rc | |
| `object_set_bool` | `obj, key: string, v: bool` | | rc | |
| `env_get` | `env, name: string` | maybe(handle) | rc | The binding `name` of the handler's `env` (handle 2): a string for a text variable or secret, the binding object otherwise; 0 if there is none. Only own properties of `env` count. |
| `array_new` | | handle | direct | `[]` |
| `array_push` | `arr, v` | | rc | |
| `array_len` | `arr` | u32 | direct | Traps if not an array. |
| `array_get` | `arr, i: u32` | handle | rc | `arr[i]` (a handle to `undefined` past the end). |
| `error_new` | `name: string, message: string` | handle | direct | `new Error(message)` with `name` set. |
| `error_name` | `err` | string | rc | `err.name` for an `Error`, else empty. |
| `error_message` | `err` | string | rc | `err.message` for an `Error`, else ToString. |
| `respond` | `response` | | rc | [respond](#respond). |
| `console_log` | `level: u32, msg: string` | | none | 0 debug, 1 log, 2 warn, 3 error. |
| `panic` | `msg: string` | | none | Logs `guest panicked: msg` with `console.error` and traps. Allowed in `init`. |

Value kinds (`value_kind`): 0 undefined, 1 null, 2 boolean, 3 number,
4 bigint, 5 string, 6 symbol, 7 function, 8 object, 9 array, 10 ArrayBuffer or view.

## Worked example: `Headers.get`

RTTI (from `Headers` in `src/workerd/api/headers.h`):

```
method get(<jsgLock>, kj::String) -> kj::Maybe<kj::String>
```

Lowering: instance method, so `self` comes first. `<jsgLock>` is omitted. `kj::String` is
`string(kj::String)`: `ptr, len`. The result `kj::Maybe<kj::String>` is `maybe(string)`, not
handle-like, so its slot is `present: u32` at 0 and a StrSlot at 8: 24 bytes, align 8. The import
returns the status.

```
(import "cf:http/v1" "Headers#get"
  (func (param $self i32) (param $name_ptr i32) (param $name_len i32) (param $ret i32)
        (result i32)))
```

A call, step by step:

1. The guest reserves a 24-byte slot at `ret` and a buffer of `cap` bytes at `buf`; it stores
   `buf` at `ret+8` and `cap` at `ret+12`. It calls `Headers#get(h, name_ptr, name_len, ret)`.
2. The host checks `h` is live and the ranges are in memory, decodes the name (UTF-8, replacement
   mode) and calls `Headers.prototype.get.call(headers, name)`.
3. If that throws (for example `TypeError: Invalid header name.`), the host returns a new handle
   to the error. Nothing is written.
4. If it returns `null`, the host writes `present = 0` at `ret` and returns 0.
5. Otherwise it encodes the value as UTF-8 (`n` bytes), writes `present = 1` at `ret` and
   `len = n` at `ret+16`; if `n <= cap` it copies the bytes to `buf` and writes `overflow = 0`
   at `ret+20`, else it writes a new handle holding the bytes to `ret+20`. It returns 0.

## Worked example: `fetch`

RTTI (the global scope's `fetch`):

```
method fetch(<jsgLock>, kj::OneOf<Request, kj::String>,
             jsg::Optional<kj::OneOf<RequestInitializerDict, Request>>) -> Promise<Response>
```

Lowering: a global function, so no `self`. The first parameter is
`oneof(handle(Request), string)`: `tag, p0, p1`, all `i32` (Request is `(0, h, 0)`, a URL string
is `(1, ptr, len)`). The second is an optional union of two handles, which collapses to
`maybe(handle)`: one `i32`, 0 for absent (passed to `fetch` as `undefined`). The result is a
promise, so the import is **suspending** and the slot holds the awaited `Response` handle
(4 bytes).

```
(import "cf:http/v1" "globalThis.fetch"
  (func (param $tag i32) (param $p0 i32) (param $p1 i32) (param $init i32) (param $ret i32)
        (result i32)))
```

A call, step by step:

1. The guest builds an init dictionary if it needs one, with imports of `cf:core/v1`:
   `o = object_new()`, `object_set_string(o, "method", "POST")`, `object_set(o, "headers", h)`,
   and so on.
2. It calls `globalThis.fetch(1, url_ptr, url_len, o, ret)`. The guest is parked from here on.
3. The host decodes the URL and calls `fetch(url, init)`. If `fetch` throws synchronously or
   the promise rejects (`TypeError: Invalid URL`, a network error), the call returns a new handle
   to the error once the promise has settled.
4. When the promise resolves, the host writes a new handle to the `Response` at `ret` and returns
   0. The guest resumes.
5. The guest reads the body with `Body#text` or `Body#bytes` (suspending). It drops the handles
   it no longer needs (`o`, the response).

## Tooling

The generator (`generator/`) is plain Python 3 with no dependencies outside the standard library.
It reads RTTI that a `workerd` binary exports through `workerd:rtti` (`generator/dump_rtti.py`
runs it with `generator/rtti-server.capnp`). The modules and their contents are listed in
`MODULES` in `generator/generate.py`, the conventions-defined imports in
`generator/core_imports.py`. The records and shim modules it writes need no formatting pass. From
the repository root:

- **Regenerate** the records (`modules/`) and shim modules (`shim/cf-*.js`) after a change to the
  generator, to an include list or to an included member:

  ```sh
  bazel build //src/workerd/server:workerd
  python3 -I src/workerd/wasm-abi/generator/generate.py \
    --workerd bazel-bin/src/workerd/server/workerd --compat-date 2026-10-23
  ```

- **Gating flags.** `python3 -I src/workerd/wasm-abi/generator/compat_gates.py --workerd WORKERD
  --old-date 2023-01-01 --new-date 2026-10-23` rewrites `generator/compat-gates.json`;
  regenerate afterwards.
- **Self-test.** `bazel test //src/workerd/wasm-abi:generator-self-test` checks that the
  checked-in modules are what the generator produces from the RTTI of the `workerd` being built
  (which includes the checks of [Versioning](#versioning), [Growth](#growth) and
  [Deprecated members](#deprecated-members)), that every included member and conventions-defined
  import is in it and nothing else is, that deprecated released imports are marked, and that the
  older date differs only in gated imports (printing the diff). Members that are not added yet
  are counted, not failed. `generator/self_test.py --help` shows how to run it
  on RTTI dumps. `//src/workerd/wasm-abi:generator-test` tests the generator's rules on
  synthetic input.
- **End to end.** `bazel test //src/workerd/wasm-abi:tests/launcher-test@` runs a guest
  (`tests/guest.wat`) that uses `cf:core/v1` and `cf:http/v1` through the launcher and the shim,
  and the launcher's error cases, with both module registries. The guest also imports what the
  first guests written against the ABI use, so a module that loses one of those imports fails
  the test.
- **JSPI.** `bazel test //src/workerd/wasm-abi:tests/jspi-test@` (and its `@all-compat-flags`
  variant) checks that the JSPI API the shim relies on exists in a worker context.

The test guests are WebAssembly text files (`tests/*.wat`), which the build assembles with
`wasm-tools parse`.

## Planned modules

- **`cf:streams/v1`**: reading and writing streams. It will be specified by hand rather than
  lowered from RTTI. Workerd is moving its Web Streams implementation from C++ to TypeScript
  (the `typescript_implemented_streams` flag, `src/per_isolate/webstreams/`), so the stream
  classes' JSG members are not a stable source to lower from. Guests also want buffer-oriented
  imports: read into guest memory, write from it, close, rather than lowered methods that pass
  chunk objects. `Body#body.get` is added to `cf:http/v1` together with it.
- **`cf:kv/v1`**: `KvNamespace`, whose binding objects the guest gets with `env_get`.

Imports that would run arbitrary JavaScript (reading any property through getters, calling any
method) are not planned. If they are ever added, they go into a module of their own
(`cf:js/v1`) that a native host never provides, so a guest that uses them is recognizably not
servable natively.

## Known deviations

Places where the shim does not, or cannot, do what this document says:

- The shim does not notify guests when the host drops a callback, and it cannot check that the
  module declares a memory maximum (`WebAssembly.Module` does not expose memory limits; the check
  belongs where the module bytes are available, at upload or load).
- A trap inside a callback that comes from calling a suspending import is raised by V8 before
  the import runs, so the import does not appear in the shim's trace.

## Open questions

Results that members not added yet would need:

- Iterators (`Headers#entries`, `keys` and `values`): `next` imports generated from the nested
  iterator types in the RTTI. (`Headers#forEach` covers headers today.)
- Arbitrary JS values (`Body#json`, `Request#cf.get`): a byte encoding, or typed lowerings.
- JSG struct results (`cf:kv/v1` will need them): a slot record, or per-field getter imports
  generated from the struct's RTTI members.

Decided provisionally, to be confirmed with real programs written against the ABI:

- The default declared memory maximum and who sets it.
- Whether dictionaries need faster forms than the literal lowering (built one property per import
  call).
- The union result rule (first accepting variant of the JS value), which a native implementation
  must reproduce even where C++ knows the variant.
- Callback lifetime notification, which the shim cannot provide.
