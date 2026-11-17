// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "i18n.h"

#include <workerd/jsg/exception.h>
#include <workerd/rust/i18n/ffi.rs.h>
#include <workerd/rust/jsg/ffi-inl.h>

#include <kj-rs/convert.h>

namespace workerd::api::node {

namespace rust_i18n = ::workerd::rust::i18n;

namespace i18n {

bool canBeTranscoded(Encoding encoding) {
  return rust_i18n::can_be_transcoded(encoding);
}

jsg::JsUint8Array transcode(
    jsg::Lock& js, kj::ArrayPtr<kj::byte> source, Encoding fromEncoding, Encoding toEncoding) {
  // The bridge's `Encoding` is this `Encoding` (see `src/rust/i18n/encoding.h`). The Rust side
  // also rejects the encodings that `canBeTranscoded` rejects.
  auto maybeLocal =
      rust_i18n::transcode(js.v8Isolate, source.as<kj_rs::Rust>(), fromEncoding, toEncoding);
  auto local =
      jsg::check(::workerd::rust::jsg::maybe_local_from_ffi<v8::Uint8Array>(kj::mv(maybeLocal)));
  return jsg::JsUint8Array(local);
}

}  // namespace i18n

}  // namespace workerd::api::node
