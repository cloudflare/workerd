// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "i18n.h"

#include "simdutf.h"

#include <workerd/jsg/exception.h>

#include <unicode/ucnv.h>
#include <unicode/uidna.h>
#include <unicode/urename.h>
#include <unicode/utypes.h>
#include <unicode/uvernum.h>
#include <unicode/uversion.h>

namespace workerd::api::node {

namespace i18n {

namespace {

// An isolate has a 128mb memory limit.
constexpr int ISOLATE_LIMIT = 134217728;

constexpr const char* getEncodingName(Encoding input) {
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

using TranscodeImpl = kj::Function<kj::Maybe<jsg::BufferSource>(
    jsg::Lock& js, kj::ArrayPtr<kj::byte> source, Encoding fromEncoding, Encoding toEncoding)>;

kj::Maybe<jsg::BufferSource> TranscodeDefault(
    jsg::Lock& js, kj::ArrayPtr<kj::byte> source, Encoding fromEncoding, Encoding toEncoding) {
  Converter to(toEncoding);
  auto substitute = kj::str(kj::repeat('?', to.minCharSize()));
  to.setSubstituteChars(substitute);
  Converter from(fromEncoding);

  size_t limit = source.size() * to.maxCharSize();
  if (limit == 0) {
    auto empty = jsg::BackingStore::alloc<v8::Uint8Array>(js, 0);
    return jsg::BufferSource(js, kj::mv(empty));
  }
  // Workers are limited to 128MB so this isn't actually a realistic concern, but sanity check.
  JSG_REQUIRE(limit <= ISOLATE_LIMIT, Error, "Source buffer is too large to transcode");

  auto out = jsg::BackingStore::alloc<v8::Uint8Array>(js, limit);
  auto outPtr = out.asArrayPtr().asChars();
  char* target = outPtr.begin();
  const char* source_ = source.asChars().begin();
  UErrorCode status{};
  ucnv_convertEx(to.conv(), from.conv(), &target, target + limit, &source_, source_ + source.size(),
      nullptr, nullptr, nullptr, nullptr, true, true, &status);
  if (U_SUCCESS(status)) {
    out.limit(target - outPtr.begin());
    return jsg::BufferSource(js, kj::mv(out));
  }

  return kj::none;
}

kj::Maybe<jsg::BufferSource> TranscodeLatin1ToUTF16(
    jsg::Lock& js, kj::ArrayPtr<kj::byte> source, Encoding fromEncoding, Encoding toEncoding) {
  auto length_in_chars = source.size() * sizeof(char16_t);
  // Workers are limited to 128MB so this isn't actually a realistic concern, but sanity check.
  JSG_REQUIRE(length_in_chars <= ISOLATE_LIMIT, Error, "Source buffer is too large to transcode");

  if (length_in_chars == 0) {
    auto empty = jsg::BackingStore::alloc<v8::Uint8Array>(js, 0);
    return jsg::BufferSource(js, kj::mv(empty));
  }

  Converter from(fromEncoding);
  auto destBuf = jsg::BackingStore::alloc<v8::Uint8Array>(js, length_in_chars);
  auto destPtr = destBuf.asArrayPtr<char16_t>();
  auto actual_length =
      simdutf::convert_latin1_to_utf16(source.asChars().begin(), source.size(), destPtr.begin());

  // simdutf returns 0 for invalid value.
  if (actual_length == 0) {
    return kj::none;
  }

  destBuf.limit(actual_length * sizeof(char16_t));
  return jsg::BufferSource(js, kj::mv(destBuf));
}

kj::Maybe<jsg::BufferSource> TranscodeFromUTF16(
    jsg::Lock& js, kj::ArrayPtr<kj::byte> source, Encoding fromEncoding, Encoding toEncoding) {
  Converter to(toEncoding);
  auto substitute = kj::str(kj::repeat('?', to.minCharSize()));
  to.setSubstituteChars(substitute);

  JSG_REQUIRE(
      source.size() % sizeof(char16_t) == 0, Error, "UTF-16le input size should be multiple of 2");
  auto utf16_input = kj::arrayPtr<char16_t>(
      reinterpret_cast<char16_t*>(source.begin()), source.size() / sizeof(char16_t));

  const auto limit = utf16_input.size() * to.maxCharSize();

  // Workers are limited to 128MB so this isn't actually a realistic concern, but sanity check.
  JSG_REQUIRE(limit <= ISOLATE_LIMIT, Error, "Buffer is too large to transcode");

  if (limit == 0) {
    auto empty = jsg::BackingStore::alloc<v8::Uint8Array>(js, 0);
    return jsg::BufferSource(js, kj::mv(empty));
  }

  auto destBuf = jsg::BackingStore::alloc<v8::Uint8Array>(js, limit);
  auto destPtr = destBuf.asArrayPtr().asChars();
  UErrorCode status{};
  auto len = ucnv_fromUChars(
      to.conv(), destPtr.begin(), destPtr.size(), utf16_input.begin(), utf16_input.size(), &status);

  if (U_SUCCESS(status)) {
    destBuf.limit(len);
    return jsg::BufferSource(js, kj::mv(destBuf));
  }

  return kj::none;
}

kj::Maybe<jsg::BufferSource> TranscodeUTF16FromUTF8(
    jsg::Lock& js, kj::ArrayPtr<kj::byte> source, Encoding fromEncoding, Encoding toEncoding) {
  size_t expected_utf16_length =
      simdutf::utf16_length_from_utf8(source.asChars().begin(), source.size());
  // Workers are limited to 128MB so this isn't actually a realistic concern, but sanity check.
  JSG_REQUIRE(expected_utf16_length <= ISOLATE_LIMIT, Error,
      "Expected UTF-16le length is too large to transcode");

  auto length_in_chars = expected_utf16_length * sizeof(char16_t);
  if (length_in_chars == 0) {
    auto empty = jsg::BackingStore::alloc<v8::Uint8Array>(js, 0);
    return jsg::BufferSource(js, kj::mv(empty));
  }

  auto destBuf = jsg::BackingStore::alloc<v8::Uint8Array>(js, length_in_chars);
  auto destPtr = destBuf.asArrayPtr<char16_t>();

  size_t actual_length =
      simdutf::convert_utf8_to_utf16le(source.asChars().begin(), source.size(), destPtr.begin());

  // simdutf returns 0 for invalid UTF-8 value.
  if (actual_length == 0) {
    return kj::none;
  }

  JSG_REQUIRE(actual_length == expected_utf16_length, Error, "Expected UTF16 length mismatch");

  return jsg::BufferSource(js, kj::mv(destBuf));
}

kj::Maybe<jsg::BufferSource> TranscodeUTF8FromUTF16(
    jsg::Lock& js, kj::ArrayPtr<kj::byte> source, Encoding fromEncoding, Encoding toEncoding) {
  JSG_REQUIRE(source.size() % 2 == 0, Error, "UTF-16le input size should be multiple of 2");
  auto utf16_input =
      kj::arrayPtr<char16_t>(reinterpret_cast<char16_t*>(source.begin()), source.size() / 2);
  size_t expected_utf8_length =
      simdutf::utf8_length_from_utf16le(utf16_input.begin(), utf16_input.size());

  // Workers are limited to 128MB so this isn't actually a realistic concern, but sanity check.
  JSG_REQUIRE(expected_utf8_length <= ISOLATE_LIMIT, Error,
      "Expected UTF-8 length is too large to transcode");

  if (expected_utf8_length == 0) {
    auto empty = jsg::BackingStore::alloc<v8::Uint8Array>(js, 0);
    return jsg::BufferSource(js, kj::mv(empty));
  }

  auto destBuf = jsg::BackingStore::alloc<v8::Uint8Array>(js, expected_utf8_length);
  auto destPtr = destBuf.asArrayPtr().asChars();

  size_t actual_length =
      simdutf::convert_utf16le_to_utf8(utf16_input.begin(), utf16_input.size(), destPtr.begin());
  JSG_REQUIRE(actual_length == expected_utf8_length, Error, "Expected UTF8 length mismatch");

  // simdutf returns 0 for invalid UTF-8 value.
  if (actual_length == 0) {
    return kj::none;
  }

  return jsg::BufferSource(js, kj::mv(destBuf));
}

}  // namespace

Converter::Converter(Encoding encoding, kj::StringPtr substitute) {
  UErrorCode status = U_ZERO_ERROR;
  auto name = getEncodingName(encoding);
  auto conv = ucnv_open(name, &status);
  JSG_REQUIRE(U_SUCCESS(status), Error, "Failed to initialize converter");
  conv_ = kj::disposeWith<ucnv_close>(conv);
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

jsg::BufferSource transcode(
    jsg::Lock& js, kj::ArrayPtr<kj::byte> source, Encoding fromEncoding, Encoding toEncoding) {
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

  return JSG_REQUIRE_NONNULL(transcode_function(js, source, fromEncoding, toEncoding), Error,
      "Unable to transcode buffer");
}

}  // namespace i18n

}  // namespace workerd::api::node
