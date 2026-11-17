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

// Maps the C++ Encoding to the Rust bridge enum, following the kj-rs
// convert.h idiom, so callers use `kj::from<rust_i18n::Encoding>(value)`. It
// is `static` (rather than in an anonymous namespace) because ADL is how
// `kj::from<Target>` locates this overload, and ADL does not search an
// unnamed namespace nested inside the argument type's namespace. The switch
// has no `default:` arm so that enum drift between the two `Encoding` types is
// a compile error.
//
// `BASE64`, `BASE64URL`, and `HEX` are not transcodable (see
// `i18n::canBeTranscoded`), so they are rejected here, for both `fromEncoding`
// and `toEncoding`. Neither is reachable from JavaScript because
// `BufferUtil::transcode` validates both encodings first.
static rust_i18n::Encoding fromImpl(rust_i18n::Encoding*, Encoding encoding) {
  switch (encoding) {
    case Encoding::ASCII:
      return rust_i18n::Encoding::Ascii;
    case Encoding::LATIN1:
      return rust_i18n::Encoding::Latin1;
    case Encoding::UTF8:
      return rust_i18n::Encoding::Utf8;
    case Encoding::UTF16LE:
      return rust_i18n::Encoding::Utf16Le;
    case Encoding::BASE64:
    case Encoding::BASE64URL:
    case Encoding::HEX:
      JSG_FAIL_REQUIRE(Error, "Invalid encoding passed to transcode");
  }
  KJ_UNREACHABLE;
}

namespace i18n {

jsg::JsUint8Array transcode(
    jsg::Lock& js, kj::ArrayPtr<kj::byte> source, Encoding fromEncoding, Encoding toEncoding) {
  auto rustFrom = kj::from<rust_i18n::Encoding>(fromEncoding);
  auto rustTo = kj::from<rust_i18n::Encoding>(toEncoding);
  auto maybeLocal = rust_i18n::transcode(js.v8Isolate, source.as<kj_rs::Rust>(), rustFrom, rustTo);
  auto local =
      jsg::check(::workerd::rust::jsg::maybe_local_from_ffi<v8::Uint8Array>(kj::mv(maybeLocal)));
  return jsg::JsUint8Array(local);
}

}  // namespace i18n

}  // namespace workerd::api::node
