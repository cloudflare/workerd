#pragma once

#include <cstdint>

namespace workerd::api::node {

// The enums in this header live in their own dependency-free header so they can
// serve as the single source of truth shared with the Rust `node-exceptions`
// crate via cxx extern enums (see `src/rust/node-exceptions/lib.rs`). cxx
// generates the Rust mirrors and static-asserts that the two definitions stay in
// correspondence. The explicit `uint8_t` underlying types keep the Rust
// `#[repr(u8)]` deterministic. Keeping them out of `exceptions.h` avoids pulling
// `jsg.h` (and a dependency cycle) into the bridge's generated header.

// Most Node.js exceptions are represented as either Error, TypeError, or
// RangeError.
enum class JsErrorType : uint8_t {
  Error,
  TypeError,
  RangeError,
};

// Node.js Exception Codes
#define NODE_EXCEPTION_CODE_LIST(V)                                                                \
  V(ERR_FS_CP_EEXIST)                                                                              \
  V(ERR_FS_CP_DIR_TO_NON_DIR)                                                                      \
  V(ERR_FS_CP_EINVAL)                                                                              \
  V(ERR_FS_CP_NON_DIR_TO_DIR)                                                                      \
  V(ERR_FS_EISDIR)

enum class NodeExceptionCode : uint8_t {
#define V(name) name,
  NODE_EXCEPTION_CODE_LIST(V)
#undef V
};

}  // namespace workerd::api::node
