// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "simdutf.h"

#include <cstddef>
#include <cstdint>

// The simdutf calls `ffi.rs` makes, forwarding to the same C++ functions `i18n.c++` calls.
// simdutf's C API (`simdutf_c.h`) would avoid this shim, but the simdutf some builds link predates
// it; the C++ functions below are present in every version we link.
//
// cxx has no `char` or `char16_t`, so the bridge passes `uint8_t` and `uint16_t` pointers. The
// UTF-16 pointers need not be aligned, exactly as in `i18n.c++`.

namespace workerd::rust::i18n {

inline size_t simdutf_convert_latin1_to_utf16(
    const uint8_t* input, size_t length, uint16_t* utf16_output) {
  return simdutf::convert_latin1_to_utf16(
      reinterpret_cast<const char*>(input), length, reinterpret_cast<char16_t*>(utf16_output));
}

inline size_t simdutf_utf16_length_from_utf8(const uint8_t* input, size_t length) {
  return simdutf::utf16_length_from_utf8(reinterpret_cast<const char*>(input), length);
}

inline size_t simdutf_convert_utf8_to_utf16le(
    const uint8_t* input, size_t length, uint16_t* utf16_output) {
  return simdutf::convert_utf8_to_utf16le(
      reinterpret_cast<const char*>(input), length, reinterpret_cast<char16_t*>(utf16_output));
}

inline size_t simdutf_utf8_length_from_utf16le(const uint16_t* input, size_t length) {
  return simdutf::utf8_length_from_utf16le(reinterpret_cast<const char16_t*>(input), length);
}

inline size_t simdutf_convert_utf16le_to_utf8(
    const uint16_t* input, size_t length, uint8_t* utf8_output) {
  return simdutf::convert_utf16le_to_utf8(
      reinterpret_cast<const char16_t*>(input), length, reinterpret_cast<char*>(utf8_output));
}

}  // namespace workerd::rust::i18n
