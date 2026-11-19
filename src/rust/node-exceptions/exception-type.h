#pragma once

#include <workerd/api/node/exception-type.h>

// Bridges the C++ `api::node::JsErrorType` and `api::node::NodeExceptionCode`
// into this crate's cxx bridge namespace so they can be used as cxx *extern*
// enums (see `lib.rs`). cxx's generated correspondence checks reference the
// enums by the bridge namespace (`workerd::rust::node_exceptions`), so the real
// types must be visible there; these aliases provide them without duplicating
// the definitions. Mirrors the `using HttpMethod = kj::HttpMethod;` pattern in
// `src/rust/kj/ffi.h`.
namespace workerd::rust::node_exceptions {
using JsErrorType = ::workerd::api::node::JsErrorType;
using NodeExceptionCode = ::workerd::api::node::NodeExceptionCode;
}  // namespace workerd::rust::node_exceptions
