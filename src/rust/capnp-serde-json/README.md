# capnp-serde-json

Rust types with serde derives, generated from Cap'n Proto schemas, whose JSON is that of the C++
`capnp::JsonCodec` with `handleByAnnotation()`. Optionally, a [cxx](../cxx/README.md) bridge per
schema lets C++ parse JSON into those types and read them in place.

The types cover JSON only. They do not read or write the Cap'n Proto binary format, and they
are not `capnp` crate readers or builders: a schema is just a description of a JSON document, and
Rust gets plain structs and enums for it.

workerd talks JSON to some services, R2 among them, through `JsonCodec` and schemas annotated
with `$Json` (`src/workerd/api/r2-api.capnp`). These types let Rust exchange the same JSON with
that C++ code and those services, and let C++ decode it without building a Cap'n Proto message.

## Contents

| Path                                         | Target                                                   | What it is                                                                   |
| -------------------------------------------- | -------------------------------------------------------- | ---------------------------------------------------------------------------- |
| `capnpc-rust-serde-json/`                    | `:capnpc-rust-serde-json`, `:capnpc-rust-serde-json-ffi` | The compiler plugin: `main.rs`, `codegen.rs` (Rust types), `ffi.rs` (bridge) |
| `lib.rs`                                     | `//src/rust/capnp-serde-json` (crate `capnp_serde_json`) | Runtime crate the generated code depends on                                  |
| `tests/`                                     | `:capnp-serde-json-test`, `:capnp-serde-json-ffi-test@`  | Tests of the generated types, and of them against the C++ codec from C++     |
| `build/wd_rust_capnp_serde_json_library.bzl` | `wd_rust_capnp_serde_json_library()`                     | Bazel macro that runs the plugin and wraps its output in a crate             |

[Compatibility](#compatibility) lists what the plugin supports and how the JSON differs from the
C++ codec's. `lib.rs`, `codegen.rs` and `ffi.rs` document the runtime crate and the mapping from
schema to Rust and to C++; this README is an overview of all three.

## Users

- `//src/workerd/api:r2-api-serde-json`, the types of the R2 binding's messages
  (`src/workerd/api/r2-api.capnp`), with the bridge.
- `//src/workerd/tests/r2-json-serde:bench-r2-json` measures decoding the R2 binding's JSON with
  `JsonCodec` against the generated types
  (`bazel run --config=benchmark //src/workerd/tests/r2-json-serde:bench-r2-json`).

## Usage

### Bazel

```python
load("//:build/wd_rust_capnp_serde_json_library.bzl", "wd_rust_capnp_serde_json_library")

wd_rust_capnp_serde_json_library(
    name = "r2-api-serde-json",       # crate r2_api_serde_json
    srcs = ["r2-api.capnp"],
    ffi = True,                       # also generate the C++ bridge
    visibility = ["//visibility:public"],
)
```

- `srcs` may import `/capnp/c++.capnp` and `/capnp/compat/json.capnp` without `deps`. Other
  imported schemas need their own `wd_rust_capnp_serde_json_library()` in `deps`, with the default
  crate name: a macro cannot read a dep's `crate_name`, so the generated `lib.rs` derives it from
  the target name.
- For `foo.capnp`, the plugin writes `foo_serde_json.rs`, which the crate's root `include!`s, so
  the types are at the crate's top level. The root imports the types of `deps` (`use dep::*`), so
  that generated code names them unqualified.
- With `ffi = True`, it also writes `foo_serde_json_ffi.rs`, the module `foo_serde_json_ffi` of
  the crate, and cxx generates the header `<pkg>/foo_serde_json_ffi.rs.h`, where `<pkg>` is the
  package's C++ include path (`workerd/api` for `src/workerd/api`). A C++ target that includes
  the header depends on the crate.
- The generated crate is linted by rustc only (`no-clippy`).

To read the generated code, build the `_gen` target. When building remotely, Bazel downloads its
outputs only if asked, with `--remote_download_regex`:

```sh
bazel build //src/workerd/api:r2-api-serde-json_gen --remote_download_regex='.*r2_api_serde_json.*'
less bazel-bin/src/workerd/api/r2_api_serde_json.rs bazel-bin/src/workerd/api/r2_api_serde_json_ffi.rs
```

### Rust

The types implement `Serialize` and `Deserialize`, so `serde_json` works with them directly.
`capnp_serde_json::from_slice()` decodes bytes with the same results as `serde_json::from_slice()`,
but faster (see [Runtime crate](#runtime-crate)):

```rust
use r2_api_serde_json::R2HeadResponse;

let head: R2HeadResponse = capnp_serde_json::from_slice(json)?;
let size = head.size;                             // u64
let name = head.name.as_deref();                  // Option<&str>
let json = serde_json::to_string(&head)?;         // as JsonCodec with HasMode::NON_DEFAULT
```

`capnp_serde_json::from_codec_input()` also ends the input at its first NUL byte, as
`JsonCodec::decode()` does.

### C++

With `ffi = True`, each struct `Foo` gets a parse function and reader-like getters:

```c++
#include <workerd/api/r2_api_serde_json_ffi.rs.h>
#include <kj-rs/convert.h>

// kj::ArrayPtr<const kj::byte> -> rust::Slice<const uint8_t>; throws if the JSON is not a Foo.
rust::Box<serde::R2HeadResponse> head = serde::parseR2HeadResponse(bytes.as<kj_rs::Rust>());
uint64_t size = head->getSize();
if (head->hasHttpFields()) {
  const serde::R2HttpFields& http = head->getHttpFields();
  kj::String contentType = kj::str(http.getContentType());          // rust::Str
}
kj::ArrayPtr<const kj::byte> md5 = kj::from<kj_rs::Rust>(head->getChecksums().getMd5());
```

See [The C++ bridge](#the-c-bridge) for the whole API.

## Schema to Rust

`codegen.rs` generates, for every declaration of a schema file:

| Cap'n Proto                                    | Rust                                                                                       |
| ---------------------------------------------- | ------------------------------------------------------------------------------------------ |
| `struct Foo`                                   | `pub struct Foo` with `Clone`, `Debug`, `PartialEq`, `Default`, `Serialize`, `Deserialize` |
| nested `struct Foo { struct Bar }`             | `pub struct FooBar` (names are joined, in PascalCase)                                      |
| unnamed union of `Foo`                         | a flattened field `which: FooWhich` holding an enum of the members                         |
| named union `bar :union $Json.flatten()`       | a flattened field `bar: FooBar` holding an enum of the members                             |
| `Void` member of an internally tagged union    | a unit variant                                                                             |
| constant of a `Bool` or integer type           | `pub const SCREAMING_SNAKE: T`                                                             |
| `enum`                                         | nothing; a field of an enum type is an error                                               |

Field types:

| Cap'n Proto                            | Rust field                | JSON                                       |
| -------------------------------------- | ------------------------- | ------------------------------------------ |
| `Bool`                                 | `bool`                    | `true` / `false`                           |
| `Int8` .. `Int32`, `UInt8` .. `UInt32` | `i8` .. `u32`             | number                                     |
| `Int64`, `UInt64`                      | `i64`, `u64`              | decimal string; a number is read too       |
| `Text`                                 | `Option<String>`          | string                                     |
| `Data $Json.base64`                    | `Option<Vec<u8>>`         | standard padded base64 string              |
| `Data $Json.hex`                       | `Option<Vec<u8>>`         | lowercase hex string; either case is read  |
| `List(T)` of integers, `Text`, structs | `Option<Vec<T>>`          | array                                      |
| struct `T`                             | `Option<T>`               | object                                     |

Pointer fields are `Option`s, `None` being Cap'n Proto's null pointer, which `null` decodes to.
Field names are snake_case, and Rust keywords become raw identifiers (`r#type`), or get a
trailing underscore where they cannot be (`self_`).

`$Json` annotations:

- `$Json.name("x")` renames a field or a union member (`#[serde(rename)]`).
- `$Json.flatten()` on a named union merges its members into the enclosing object
  (`#[serde(flatten)]`). A struct's unnamed union is always flattened, as in the C++ codec.
- `$Json.discriminator(name = "kind")` on a named union makes it internally tagged
  (`#[serde(tag = "kind")]`): the member's name is the value of `"kind"`, and the fields of a
  member, a struct with `$Json.flatten()`, sit next to it. A union without it is externally
  tagged: the member's name is its key.
- `$Json.base64` and `$Json.hex` select those encodings of `Data`, which needs one of them.

Missing fields take their schema defaults, including non-zero defaults of primitive fields
(`limit @0 :UInt32 = 0xffffffff`), through `#[serde(default)]` on each struct and the struct's
`Default` impl. Fields holding their schema defaults are left out when serializing, as
`JsonCodec` does with `HasMode::NON_DEFAULT`. Unknown keys are ignored.

For example, from `r2-api.capnp`:

```capnp
struct R2Etag {
  value @0 :Text;
  type :union $Json.flatten() $Json.discriminator(name="type") {
    strong @1 :Void;
    weak @2 :Void;
    wildcard @3 :Void;
  }
}
```

becomes (attributes abridged)

```rust
#[serde(default)]
pub struct R2Etag {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub value: Option<String>,
    #[serde(flatten)]
    pub r#type: R2EtagType,
}

#[serde(tag = "type")]
pub enum R2EtagType {
    #[serde(rename = "strong")]
    Strong,
    #[serde(rename = "weak")]
    Weak,
    #[serde(rename = "wildcard")]
    Wildcard,
}
```

which reads and writes `{"value": "abc", "type": "weak"}`.

## Runtime crate

The generated code calls into `capnp_serde_json` for what serde does not do itself:

- Markers for `#[serde(with = "::capnp_serde_json::As::<M>")]`: `Int64`, `Base64` and `Hex`, which
  implement `SerializeAs` and `DeserializeAs` for the encodings above. They compose through
  `Option<M>` and `Vec<M>`, so a `List(UInt64)` field is `Option<Vec<u64>>` with
  `As::<Option<Vec<Int64>>>`.
- The `skip_serializing_if` predicates `is_default`, `eq_bool` and `eq_i8` .. `eq_u64`, which
  compare a field with its schema default.
- `from_slice()` and `from_codec_input()`.

`from_slice()` decodes with the same results as `serde_json::from_slice()`, but checks the input's
UTF-8 once and then decodes it as a `str`. `serde_json::from_slice()` checks each string as it
reads it, which takes longer for the many short strings and keys of these documents. Input that
is not UTF-8 is handed to `serde_json::from_slice()`, which reports where the error is, or accepts
it if the invalid bytes are in a value the type ignores.

`from_codec_input()` decodes the input up to its first NUL byte, as `JsonCodec::decode()` does, so
that it may be a `kj::String`'s array with its terminator.

## The C++ bridge

`ffi.rs` generates the bridge of a schema file. Every struct and named union is an opaque Rust
type in the C++ namespace of the file's `$Cxx.namespace` followed by `::serde` (so
`workerd::api::public_beta::serde::R2HeadResponse`), which keeps the names apart from those of
`capnpc-c++`'s classes; a file without the annotation uses `capnp_serde_json::<file>`. The API
follows that of Cap'n Proto's C++ readers. For a struct `Foo`:

| C++                                                   | Rust                                                                     |
| ----------------------------------------------------- | ------------------------------------------------------------------------ |
| `rust::Box<Foo> parseFoo(rust::Slice<const uint8_t>)` | decodes with `from_codec_input()`; throws `kj::Exception` on bad JSON    |
| `getX()` of `Bool` and integer fields                 | the value                                                                |
| `getX()` of `Text`                                    | `rust::Str` into the value, empty if null                                |
| `getX()` of `Data`, lists of integers                 | `rust::Slice<const T>` into the value, empty if null                     |
| `getX()` of `List(Text)`, `List(Struct)`              | `rust::Slice<const rust::String>`, `rust::Slice<const T>`                |
| `getX()` of a struct field                            | `const T&`, to a static default value if null                            |
| `getX()` of a named union                             | `const T&` to the union                                                  |
| `hasX()` of a pointer field                           | whether it is non-null                                                   |
| `which()`, `isX()` of a union                         | a shared enum `<Union>Which`, and whether member `X` is set              |
| `getX()` of a union member                            | the member's value, or its default if another member is set              |

A struct's unnamed union has `which()`, `isX()` and `getX()` on the struct itself. A
`rust::Str` converts with `kj::str()` or `kj::from<kj_rs::Rust>()`, a `rust::Slice` with
`kj::from<kj_rs::Rust>()`.

C++ cannot read values whose types other schema files declare; the bridge has a comment in place
of each such accessor. The getters are inherent methods private to the bridge's module, so they
do not add to the Rust types' API. The bridge lives in the types' crate because cxx must
implement its traits for them.

The compiler passes plugins no options, so the plugin is built twice: `:capnpc-rust-serde-json`
writes the Rust types, and `:capnpc-rust-serde-json-ffi`, built with
`CAPNPC_RUST_SERDE_JSON_FFI` set, writes the bridges too. `ffi = True` selects the latter.

## Compatibility

The plugin supports what the schemas it is used for need, and fails with an error naming the
declaration for anything else; a schema that needs more extends the plugin. The types are
idiomatic serde, not a port of every behaviour of `JsonCodec`.

Legend: ✅ supported, ❌ the plugin fails with an error, ➖ ignored (nothing is generated).

### Schema

| Construct                                                           | Supported | Notes                                                                        |
| ------------------------------------------------------------------- | --------- | ---------------------------------------------------------------------------- |
| **Types**                                                           |           |                                                                              |
| `Bool`                                                              | ✅        |                                                                              |
| `Int8` .. `Int32`, `UInt8` .. `UInt32`                              | ✅        | JSON number                                                                  |
| `Int64`, `UInt64`                                                   | ✅        | decimal string                                                               |
| `Float32`, `Float64`                                                | ❌        |                                                                              |
| `Text`                                                              | ✅        |                                                                              |
| `Data $Json.base64`, `Data $Json.hex`                               | ✅        |                                                                              |
| `Data` without an encoding annotation                               | ❌        | C++ writes an array of numbers                                               |
| `List(T)` of integers, `Text` or structs                            | ✅        |                                                                              |
| `List(T)` of `Bool`, floats, `Data`, enums, lists, `Void`           | ❌        |                                                                              |
| structs                                                             | ✅        | also recursive through a list (`children @0 :List(Self)`)                    |
| a struct field whose type contains the struct                       | ❌        | its Rust type would have infinite size                                       |
| enum fields                                                         | ❌        |                                                                              |
| `Void` fields                                                       | ❌        | `Void` is only a member of an internally tagged union                        |
| `Json.Value`                                                        | ❌        |                                                                              |
| interfaces, `AnyPointer`                                            | ❌        | declarations too                                                             |
| generic structs                                                     | ❌        | declarations too                                                             |
| **Declarations**                                                    |           |                                                                              |
| nested structs                                                      | ✅        | `Foo.Bar` is `FooBar`                                                        |
| enum declarations                                                   | ➖        | allowed, so that a schema may declare enums it does not use in fields        |
| constants of `Bool` and integer types                               | ✅        |                                                                              |
| constants of other types                                            | ❌        |                                                                              |
| `using` aliases                                                     | ➖        | a `CodeGeneratorRequest` does not describe them                              |
| imports of other schemas                                            | ✅        | through the macro's `deps`, untested; the C++ bridge cannot read their types |
| **Defaults**                                                        |           |                                                                              |
| defaults of `Bool` and integer fields                               | ✅        | `limit @0 :UInt32 = 0xffffffff`                                              |
| defaults of pointer fields (`Text`, `Data`, lists, structs)         | ❌        |                                                                              |
| defaults of union members                                           | ❌        |                                                                              |
| **Groups and unions**                                               |           |                                                                              |
| a struct's unnamed union                                            | ✅        | externally tagged, flattened into the struct                                 |
| named union with `$Json.flatten()`                                  | ✅        | flattened into the struct; tagged if it has `$Json.discriminator`            |
| named union without `$Json.flatten()`                               | ❌        |                                                                              |
| groups that are not unions                                          | ❌        |                                                                              |
| groups as union members                                             | ❌        |                                                                              |
| member of an internally tagged union: `Void`                        | ✅        | written as the tag alone                                                     |
| member of an internally tagged union: struct with `$Json.flatten()` | ✅        | its fields sit next to the tag                                               |
| member of an internally tagged union: anything else                 | ❌        |                                                                              |
| member of an externally tagged union: any supported type            | ✅        | `Void` and flattened members are not supported                               |
| **Annotations**                                                     |           |                                                                              |
| `$Json.name` on fields and union members                            | ✅        |                                                                              |
| `$Json.flatten()` on named unions and tagged union members          | ✅        |                                                                              |
| `$Json.flatten()` on struct fields, groups, untagged members        | ❌        |                                                                              |
| `$Json.flatten(prefix = ...)`                                       | ❌        |                                                                              |
| `$Json.discriminator(name = ...)` on a named union                  | ✅        |                                                                              |
| `$Json.discriminator` without `name`, or with `valueName`           | ❌        |                                                                              |
| `$Json.discriminator` on a struct                                   | ❌        | name the union instead                                                       |
| `$Json.base64`, `$Json.hex` on `Data`                               | ✅        |                                                                              |
| `$Json.base64`, `$Json.hex` on other types                          | ❌        |                                                                              |
| `$Cxx.namespace`                                                    | ✅        | the C++ bridge's namespace, followed by `::serde`                            |
| other annotations                                                   | ➖        |                                                                              |

### JSON

JSON that `JsonCodec` writes decodes to the same values in Rust, and JSON that Rust writes decodes
to the same message in C++. The rows below where the two differ are about JSON that only other
writers produce, except for the first two, which are about representation.

| Input or output                                           | C++ `JsonCodec`                          | Rust                                                      |
| --------------------------------------------------------- | ---------------------------------------- | --------------------------------------------------------- |
| fields that hold their defaults, when writing             | written by default (`HasMode::NON_NULL`) | left out, as with `HasMode::NON_DEFAULT`                  |
| pointer union member selected without a value             | null pointer                             | empty value; both read and write the same                 |
| missing field                                             | schema default                           | schema default                                            |
| unknown key                                               | ignored                                  | ignored                                                   |
| `null` for a pointer field                                | null pointer                             | `None`                                                    |
| repeated key                                              | last value wins                          | error                                                     |
| `Int64`, `UInt64` as a JSON number                        | read as a double, so rounded above 2^53  | read exactly                                              |
| integers in strings: `" 5"`, `"0x10"`, `"5"` for `Int32`  | accepted (`strtoll()`)                   | error; only `Int64`, `UInt64` take decimal strings        |
| integral floats for integers (`5.0`)                      | accepted                                 | error                                                     |
| base64 or hex with characters outside the alphabet        | skipped, or read as 0                    | error                                                     |
| hex in upper case                                         | accepted                                 | accepted                                                  |
| externally tagged union without a member                  | its first member, with its default       | error                                                     |
| externally tagged union with several members              | the last one                             | the first one; the others are ignored, `null` or not      |
| `null` for a pointer member of an externally tagged union | key ignored                              | error, if it is the first member in the input             |
| internally tagged union without its tag                   | its first member                         | error                                                     |
| tag that names no member                                  | ignored                                  | error                                                     |
| tag that is a number                                      | error                                    | the member with that index                                |
| tag after the member's fields                             | accepted                                 | accepted                                                  |
| array of a struct's fields in order                       | error                                    | accepted, for structs without a union                     |
| unpaired UTF-16 surrogate escape (`"\ud800"`)             | invalid UTF-8 in the `Text`              | error                                                     |
| nesting depth                                             | up to 64 levels                          | up to 128 levels                                          |
| input up to its first NUL byte                            | `decode()` stops there                   | `from_codec_input()` stops there; `from_slice()` does not |

## Tests

```sh
bazel test //src/rust/capnp-serde-json/...
```

- `capnp-serde-json_test` (`lib-test.rs`): the runtime crate's markers, predicates and decoding.
- `capnpc-rust-serde-json_test` (`codegen-test.rs`, `names-test.rs`): output paths and name
  conversion.
- `capnp-serde-json-test` (`tests/capnp-serde-json-test.rs`): the generated types of
  `tests/test.capnp`, which has every construct the plugin supports. Each case is a JSON document
  as `JsonCodec` writes it by default; the Rust type decodes it, and the case checks the value and
  the JSON the type writes, which the type must decode to the same value. It also checks what
  Rust rejects.
- `capnp-serde-json-ffi-test@` (`tests/capnp-serde-json-ffi-test.c++`): every construct of
  `tests/test.capnp` read from C++ through the bridge, against `JsonCodec`. Every document the
  bridge decodes, `JsonCodec` must decode to a message whose fields read the same, and
  `JsonCodec` with `HasMode::NON_DEFAULT` must write the JSON that `capnp-serde-json-test`
  expects of the Rust types.

A change to the generator should come with a construct in `tests/test.capnp` and cases in both
tests; the C++ test holds the same documents as the Rust one.
