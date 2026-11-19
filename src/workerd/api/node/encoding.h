// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <cstdint>

namespace workerd::api::node {

using EncodingValue = uint8_t;

// The encodings of `node-internal:buffer`, which passes them as these numbers.
//
// This enum lives in its own dependency-free header so it can serve as the
// single source of truth shared with the Rust `i18n` crate via a cxx extern
// enum (see `src/rust/i18n/ffi.rs`). cxx generates the Rust mirror and
// static-asserts that the two definitions stay in correspondence.
enum Encoding : EncodingValue {
  ASCII = 0,
  LATIN1 = 1,
  UTF8 = 2,
  UTF16LE = 3,
  BASE64 = 4,
  BASE64URL = 5,
  HEX = 6,
};

}  // namespace workerd::api::node
