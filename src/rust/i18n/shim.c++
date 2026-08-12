// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "shim.h"

#include "simdutf.h"

#include <unicode/ucnv.h>

#include <kj/debug.h>

#include <string>

namespace workerd::rust::i18n {

Converter::~Converter() noexcept {
  if (conv_ != nullptr) {
    ucnv_close(conv_);
  }
}

size_t Converter::max_char_size() const {
  return static_cast<size_t>(ucnv_getMaxCharSize(conv_));
}

size_t Converter::min_char_size() const {
  return static_cast<size_t>(ucnv_getMinCharSize(conv_));
}

void Converter::set_subst_chars(::rust::Str substitute) const {
  if (substitute.size() == 0) return;
  UErrorCode status = U_ZERO_ERROR;
  ucnv_setSubstChars(conv_, substitute.data(), static_cast<int8_t>(substitute.size()), &status);
  // Unreachable in practice: the substitute strings this module sets are always
  // short, valid ASCII ('?' repeated `minCharSize()` times), so ICU never
  // rejects them for any of the four transcodable encodings.
  KJ_REQUIRE(U_SUCCESS(status), "Setting ICU substitute characters failed");
}

std::unique_ptr<Converter> open_converter(::rust::Str name) {
  UErrorCode status = U_ZERO_ERROR;
  std::string nameStr(name.data(), name.size());
  auto* conv = ucnv_open(nameStr.c_str(), &status);
  // Unreachable in practice: this module only ever opens converters for the
  // four fixed, always-valid ICU encoding names ("us-ascii", "iso8859-1",
  // "utf-8", "utf16le").
  KJ_REQUIRE(U_SUCCESS(status), "Failed to initialize converter");
  return std::make_unique<Converter>(conv);
}

int64_t convert_ex(const Converter& to,
    const Converter& from,
    ::rust::Slice<const uint8_t> source,
    ::rust::Slice<uint8_t> target) {
  char* const targetStart = reinterpret_cast<char*>(target.data());
  char* targetPtr = targetStart;
  const char* sourcePtr = reinterpret_cast<const char*>(source.data());
  UErrorCode status = U_ZERO_ERROR;
  ucnv_convertEx(to.conv_, from.conv_, &targetPtr, targetStart + target.size(), &sourcePtr,
      sourcePtr + source.size(), nullptr, nullptr, nullptr, nullptr, true, true, &status);
  if (U_FAILURE(status)) return -1;
  return static_cast<int64_t>(targetPtr - targetStart);
}

int64_t from_uchars(
    const Converter& to, ::rust::Slice<const uint8_t> source, ::rust::Slice<uint8_t> target) {
  UErrorCode status = U_ZERO_ERROR;
  auto len = ucnv_fromUChars(to.conv_, reinterpret_cast<char*>(target.data()),
      static_cast<int32_t>(target.size()), reinterpret_cast<const UChar*>(source.data()),
      static_cast<int32_t>(source.size() / sizeof(UChar)), &status);
  if (U_FAILURE(status)) return -1;
  return static_cast<int64_t>(len);
}

size_t convert_latin1_to_utf16(::rust::Slice<const uint8_t> source, ::rust::Slice<uint8_t> target) {
  return simdutf::convert_latin1_to_utf16(reinterpret_cast<const char*>(source.data()),
      source.size(), reinterpret_cast<char16_t*>(target.data()));
}

size_t utf16_length_from_utf8(::rust::Slice<const uint8_t> source) {
  return simdutf::utf16_length_from_utf8(
      reinterpret_cast<const char*>(source.data()), source.size());
}

size_t convert_utf8_to_utf16le(::rust::Slice<const uint8_t> source, ::rust::Slice<uint8_t> target) {
  return simdutf::convert_utf8_to_utf16le(reinterpret_cast<const char*>(source.data()),
      source.size(), reinterpret_cast<char16_t*>(target.data()));
}

size_t utf8_length_from_utf16le(::rust::Slice<const uint8_t> source) {
  return simdutf::utf8_length_from_utf16le(
      reinterpret_cast<const char16_t*>(source.data()), source.size() / sizeof(char16_t));
}

size_t convert_utf16le_to_utf8(::rust::Slice<const uint8_t> source, ::rust::Slice<uint8_t> target) {
  return simdutf::convert_utf16le_to_utf8(reinterpret_cast<const char16_t*>(source.data()),
      source.size() / sizeof(char16_t), reinterpret_cast<char*>(target.data()));
}

}  // namespace workerd::rust::i18n
