// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// WHATWG-compliant legacy decoders (CJK multi-byte, windows-1252,
// x-user-defined) implemented via the encoding_rs Rust crate through
// a CXX bridge. A single LegacyDecoder class wraps an opaque Rust-side
// decoder that handles all the encoding-specific state machines.

#pragma once

#include "encoding-shared.h"

#include <workerd/rust/encoding/lib.rs.h>

#include <rust/cxx.h>

#include <kj/common.h>

namespace workerd::api {

// Unified legacy decoder using encoding_rs via Rust CXX bridge.
// encoding_rs implements the full WHATWG decoder algorithms for all
// legacy encodings, including streaming, error recovery, and ASCII
// byte pushback.
//
// According to WHATWG spec, any encoding except UTF-8 and UTF-16 is considered legacy.
class LegacyDecoder final: public Decoder {
 public:
  LegacyDecoder(Encoding encoding, DecoderFatal fatal);
  ~LegacyDecoder() noexcept = default;
  LegacyDecoder(LegacyDecoder&&) noexcept = default;
  LegacyDecoder& operator=(LegacyDecoder&&) noexcept = default;
  KJ_DISALLOW_COPY(LegacyDecoder);

  Encoding getEncoding() override {
    return encoding;
  }

  kj::Maybe<jsg::JsString> decode(
      jsg::Lock& js, kj::ArrayPtr<const kj::byte> buffer, bool flush = false) override;

  void reset() override;

 private:
  Encoding encoding;
  DecoderFatal fatal;
  ::rust::Box<::workerd::rust::encoding::Decoder> state;
};

}  // namespace workerd::api
