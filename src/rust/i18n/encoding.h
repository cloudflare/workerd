// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/node/encoding.h>

// Bridges the C++ `api::node::Encoding` into this crate's cxx bridge namespace so
// it can be used as a cxx *extern* enum (see `ffi.rs`). cxx's generated
// correspondence checks reference the enum by the bridge namespace
// (`workerd::rust::i18n`), so the real type must be visible there; this alias
// provides it without duplicating the definition. Mirrors
// `src/rust/node-exceptions/exception-type.h`.
namespace workerd::rust::i18n {
using Encoding = ::workerd::api::node::Encoding;
}  // namespace workerd::rust::i18n
