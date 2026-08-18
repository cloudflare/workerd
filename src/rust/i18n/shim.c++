// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "shim.h"

#include "simdutf.h"

#include <unicode/ucnv.h>

#include <kj/string.h>

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

bool Converter::set_subst_chars(::rust::Str substitute) const {
  if (substitute.empty()) return true;
  // `ucnv_setSubstChars` takes the length as an `int8_t`, and reads a negative
  // length as "NUL-terminated", which `rust::Str` is not. Reject anything that
  // would not survive the narrowing; ICU's own limit is far lower still.
  if (substitute.size() > INT8_MAX) return false;
  UErrorCode status = U_ZERO_ERROR;
  ucnv_setSubstChars(conv_, substitute.data(), static_cast<int8_t>(substitute.size()), &status);
  return U_SUCCESS(status);
}

std::unique_ptr<Converter> open_converter(::rust::Str name) {
  UErrorCode status = U_ZERO_ERROR;
  // `ucnv_open` needs a NUL-terminated name, which `rust::Str` is not.
  auto nameStr = kj::str(kj::ArrayPtr<const char>(name.data(), name.size()));
  auto* conv = ucnv_open(nameStr.cStr(), &status);
  if (U_FAILURE(status)) return nullptr;
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
