#pragma once

#include <workerd/api/node/exception-type.h>

// Bridges the C++ `api::node::JsErrorType` into this crate's cxx bridge
// namespace so it can be used as a cxx *extern* enum (see `lib.rs`). cxx's
// generated correspondence checks reference the enum by the bridge namespace
// (`workerd::rust::node_exceptions`), so the real type must be visible there;
// this alias provides it without duplicating the definition. Mirrors the
// `using HttpMethod = kj::HttpMethod;` pattern in `src/rust/kj/ffi.h`.
namespace workerd::rust::node_exceptions {
using JsErrorType = ::workerd::api::node::JsErrorType;
}  // namespace workerd::rust::node_exceptions
