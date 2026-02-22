// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "encoding-legacy.h"

#include <kj-rs/convert.h>
#include <rust/cxx.h>

#include <kj/common.h>

namespace workerd::api {

namespace {

// Map workerd::api::Encoding to the Rust-side RustEncoding enum.
::workerd::rust::encoding::Encoding toRustEncoding(Encoding encoding) {
  using RE = ::workerd::rust::encoding::Encoding;
  switch (encoding) {
    case Encoding::Big5:
      return RE::Big5;
    case Encoding::Euc_Jp:
      return RE::EucJp;
    case Encoding::Euc_Kr:
      return RE::EucKr;
    case Encoding::Gb18030:
      return RE::Gb18030;
    case Encoding::Gbk:
      return RE::Gbk;
    case Encoding::Iso2022_Jp:
      return RE::Iso2022Jp;
    case Encoding::Shift_Jis:
      return RE::ShiftJis;
    case Encoding::Windows_1252:
      return RE::Windows1252;
    case Encoding::X_User_Defined:
      return RE::XUserDefined;
    default:
      KJ_UNREACHABLE;
  }
}

}  // namespace

LegacyDecoder::LegacyDecoder(Encoding encoding, DecoderFatal fatal)
    : encoding(encoding),
      fatal(fatal),
      state(::workerd::rust::encoding::new_decoder(toRustEncoding(encoding))) {}

void LegacyDecoder::reset() {
  ::workerd::rust::encoding::reset(*state);
}

kj::Maybe<jsg::JsString> LegacyDecoder::decode(
    jsg::Lock& js, kj::ArrayPtr<const kj::byte> buffer, bool flush) {
  auto result = ::workerd::rust::encoding::decode(
      *state, buffer.as<kj_rs::RustMutable>(), flush, fatal.toBool());

  if (fatal.toBool() && result.had_error) {
    // Decoder state already reset by the Rust side on fatal error.
    return kj::none;
  }

  auto output = kj::from<kj_rs::Rust>(result.output);
  return js.str(output);
}

}  // namespace workerd::api
