#pragma once

#include <cstdint>

namespace workerd::api::node {

// Most Node.js exceptions are represented as either Error, TypeError, or
// RangeError.
//
// This enum lives in its own dependency-free header so it can serve as the
// single source of truth shared with the Rust `node-exceptions` crate via a cxx
// extern enum (see `src/rust/node-exceptions/lib.rs`). cxx generates the Rust
// mirror and static-asserts that the two definitions stay in correspondence.
// The explicit `uint8_t` underlying type keeps the Rust `#[repr(u8)]`
// deterministic. Keeping this out of `exceptions.h` avoids pulling `jsg.h` (and
// a dependency cycle) into the bridge's generated header.
enum class JsErrorType : uint8_t {
  Error,
  TypeError,
  RangeError,
};

}  // namespace workerd::api::node
