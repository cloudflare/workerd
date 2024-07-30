// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "i18n.h"

#include <workerd/jsg/exception.h>

#include <unicode/putil.h>
#include <unicode/uchar.h>
#include <unicode/uclean.h>
#include <unicode/ucnv.h>
#include <unicode/udata.h>
#include <unicode/uidna.h>
#include <unicode/urename.h>
#include <unicode/ustring.h>
#include <unicode/utf16.h>
#include <unicode/utf8.h>
#include <unicode/utypes.h>
#include <unicode/uvernum.h>
#include <unicode/uversion.h>

#include "simdutf.h"

namespace workerd::api::node {

namespace i18n {

namespace {

// An isolate has a 128mb memory limit.
const int ISOLATE_LIMIT = 134217728;

struct ConverterDisposer : public kj::Disposer {
  static const ConverterDisposer INSTANCE;
  void disposeImpl(void* pointer) const override {
    ucnv_close(reinterpret_cast<UConverter*>(pointer));
  }
};

const ConverterDisposer ConverterDisposer::INSTANCE;

const char* getEncodingName(Encoding input) {
  switch (input) {
  case Encoding::ASCII:
    return "us-ascii";
  case Encoding::LATIN1:
    return "iso8859-1";
  case Encoding::UTF16LE:
    return "utf16le";
  case Encoding::UTF8:
    return "utf-8";
  default:
    KJ_UNREACHABLE;
  }
}

typedef kj::Maybe<kj::Array<kj::byte>> (*TranscodeImpl)(kj::ArrayPtr<kj::byte> source,
                                                        Encoding fromEncoding, Encoding toEncoding);

kj::Maybe<kj::Array<kj::byte>> TranscodeDefault(kj::ArrayPtr<kj::byte> source,
                                                Encoding fromEncoding, Encoding toEncoding) {
  Converter to(toEncoding);
  auto substitute = kj::str(kj::repeat('?', to.minCharSize()));
  to.setSubstituteChars(substitute);
  Converter from(fromEncoding);

  size_t limit = source.size() * to.maxCharSize();
  // Workers are limited to 128MB so this isn't actually a realistic concern, but sanity check.
  JSG_REQUIRE(limit <= ISOLATE_LIMIT, Error, "Source buffer is too large to transcode");
  auto out = kj::heapArray<kj::byte>(limit);
  char* target = out.asChars().begin();
  const char* source_ = source.asChars().begin();
  UErrorCode status{};
  ucnv_convertEx(to.conv(), from.conv(), &target, target + limit, &source_, source_ + source.size(),
                 nullptr, nullptr, nullptr, nullptr, true, true, &status);
  if (U_SUCCESS(status)) {
    return out.slice(0, target - out.asChars().begin()).attach(kj::mv(out));
  }

  return kj::none;
}

kj::Maybe<kj::Array<kj::byte>> TranscodeLatin1ToUTF16(kj::ArrayPtr<kj::byte> source,
                                                      Encoding fromEncoding, Encoding toEncoding) {
  auto length_in_chars = source.size() * sizeof(UChar);
  // Workers are limited to 128MB so this isn't actually a realistic concern, but sanity check.
  JSG_REQUIRE(length_in_chars <= ISOLATE_LIMIT, Error, "Source buffer is too large to transcode");

  Converter from(fromEncoding);
  auto destbuf = kj::heapArray<UChar>(length_in_chars);
  auto actual_length =
      simdutf::convert_latin1_to_utf16(source.asChars().begin(), source.size(), destbuf.begin());

  // simdutf returns 0 for invalid value.
  if (actual_length == 0) {
    return kj::none;
  }

  return destbuf.slice(0, actual_length).asBytes().attach(kj::mv(destbuf));
}

kj::Maybe<kj::Array<kj::byte>> TranscodeFromUTF16(kj::ArrayPtr<kj::byte> source,
                                                  Encoding fromEncoding, Encoding toEncoding) {
  Converter to(toEncoding);
  auto substitute = kj::str(kj::repeat('?', to.minCharSize()));
  to.setSubstituteChars(substitute);

  auto utf16_input = kj::arrayPtr<char16_t>(reinterpret_cast<char16_t*>(source.begin()),
                                            source.size() / sizeof(UChar));

  const auto limit = utf16_input.size() * to.maxCharSize();

  // Workers are limited to 128MB so this isn't actually a realistic concern, but sanity check.
  JSG_REQUIRE(limit <= ISOLATE_LIMIT, Error, "Buffer is too large to transcode");

  auto destbuf = kj::heapArray<UChar>(limit);
  UErrorCode status{};
  auto len = ucnv_fromUChars(to.conv(), destbuf.asChars().begin(), destbuf.size(),
                             utf16_input.begin(), utf16_input.size(), &status);

  if (U_SUCCESS(status)) {
    return destbuf.slice(0, len).asBytes().attach(kj::mv(destbuf));
  }

  return kj::none;
}

kj::Maybe<kj::Array<kj::byte>> TranscodeUTF16FromUTF8(kj::ArrayPtr<kj::byte> source,
                                                      Encoding fromEncoding, Encoding toEncoding) {
  size_t expected_utf16_length =
      simdutf::utf16_length_from_utf8(source.asChars().begin(), source.size());
  // Workers are limited to 128MB so this isn't actually a realistic concern, but sanity check.
  JSG_REQUIRE(expected_utf16_length <= ISOLATE_LIMIT, Error,
              "Expected UTF-16le length is too large to transcode");
  auto destbuf = kj::heapArray<UChar>(expected_utf16_length);

  size_t actual_length =
      simdutf::convert_utf8_to_utf16le(source.asChars().begin(), source.size(), destbuf.begin());
  JSG_REQUIRE(actual_length == expected_utf16_length, Error, "Expected UTF16 length mismatch");

  // simdutf returns 0 for invalid UTF-8 value.
  if (actual_length == 0) {
    return kj::none;
  }

  return destbuf.asBytes().attach(kj::mv(destbuf));
}

kj::Maybe<kj::Array<kj::byte>> TranscodeUTF8FromUTF16(kj::ArrayPtr<kj::byte> source,
                                                      Encoding fromEncoding, Encoding toEncoding) {
  JSG_REQUIRE(source.size() % 2 == 0, Error, "UTF-16le input size should be multiple of 2");
  auto utf16_input =
      kj::arrayPtr<char16_t>(reinterpret_cast<char16_t*>(source.begin()), source.size() / 2);
  size_t expected_utf8_length =
      simdutf::utf8_length_from_utf16le(utf16_input.begin(), utf16_input.size());

  // Workers are limited to 128MB so this isn't actually a realistic concern, but sanity check.
  JSG_REQUIRE(expected_utf8_length <= ISOLATE_LIMIT, Error,
              "Expected UTF-8 length is too large to transcode");

  auto destbuf = kj::heapArray<kj::byte>(expected_utf8_length);

  size_t actual_length = simdutf::convert_utf16le_to_utf8(utf16_input.begin(), utf16_input.size(),
                                                          destbuf.asChars().begin());
  JSG_REQUIRE(actual_length == expected_utf8_length, Error, "Expected UTF8 length mismatch");

  // simdutf returns 0 for invalid UTF-8 value.
  if (actual_length == 0) {
    return kj::none;
  }

  return destbuf.asBytes().attach(kj::mv(destbuf));
}

} // namespace

Converter::Converter(Encoding encoding, kj::StringPtr substitute) {
  UErrorCode status = U_ZERO_ERROR;
  auto name = getEncodingName(encoding);
  auto conv = ucnv_open(name, &status);
  JSG_REQUIRE(U_SUCCESS(status), Error, "Failed to initialize converter");
  conv_ = kj::Own<UConverter>(conv, ConverterDisposer::INSTANCE);
  setSubstituteChars(substitute);
}

UConverter* Converter::conv() const {
  return const_cast<UConverter*>(conv_.get());
}

size_t Converter::maxCharSize() const {
  KJ_ASSERT_NONNULL(conv_.get());
  return ucnv_getMaxCharSize(conv_.get());
}

size_t Converter::minCharSize() const {
  KJ_ASSERT_NONNULL(conv_.get());
  return ucnv_getMinCharSize(conv_.get());
}

void Converter::reset() {
  KJ_ASSERT_NONNULL(conv_.get());
  ucnv_reset(conv_.get());
}

void Converter::setSubstituteChars(kj::StringPtr sub) {
  KJ_ASSERT_NONNULL(conv_.get());
  UErrorCode status = U_ZERO_ERROR;
  if (sub.size() > 0) {
    ucnv_setSubstChars(conv_.get(), sub.begin(), sub.size(), &status);
    JSG_REQUIRE(U_SUCCESS(status), Error, "Setting ICU substitute characters failed");
  }
}

kj::Array<kj::byte> transcode(kj::ArrayPtr<kj::byte> source, Encoding fromEncoding,
                              Encoding toEncoding) {
  // Optimization:
  // If both encodings are same, we just return a copy of the buffer.
  if (fromEncoding == toEncoding) {
    auto destbuf = kj::heapArray<kj::byte>(source.size());
    destbuf.asPtr().copyFrom(source);
    return destbuf.asBytes().attach(kj::mv(destbuf));
  }

  TranscodeImpl transcode_function = &TranscodeDefault;
  switch (fromEncoding) {
  case Encoding::ASCII:
  case Encoding::LATIN1:
    if (toEncoding == Encoding::UTF16LE) {
      transcode_function = &TranscodeLatin1ToUTF16;
    }
    break;
  case Encoding::UTF8:
    if (toEncoding == Encoding::UTF16LE) {
      transcode_function = &TranscodeUTF16FromUTF8;
    }
    break;
  case Encoding::UTF16LE:
    switch (toEncoding) {
    case Encoding::UTF16LE:
      transcode_function = &TranscodeDefault;
      break;
    case Encoding::UTF8:
      transcode_function = &TranscodeUTF8FromUTF16;
      break;
    default:
      transcode_function = &TranscodeFromUTF16;
    }
    break;
  default:
    JSG_FAIL_REQUIRE(Error, "Invalid encoding passed to transcode");
  }

  return JSG_REQUIRE_NONNULL(transcode_function(source, fromEncoding, toEncoding), Error,
                             "Unable to transcode buffer");
}

} // namespace i18n

} // namespace workerd::api::node
