// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "simdutf.h"

#include <cstddef>
#include <cstdint>

// The simdutf calls `text_encoder.rs` makes, forwarding to what `api/encoding.c++` calls. cxx has
// no `char` or `char16_t`, so the bridge passes `uint8_t` and `uint16_t` pointers.

namespace workerd::rust::encoding {

// The length of the longest ASCII prefix of `input`.
inline size_t simdutf_ascii_prefix_length(const uint8_t* input, size_t length) {
  return simdutf::validate_ascii_with_errors(reinterpret_cast<const char*>(input), length).count;
}

inline size_t simdutf_utf8_length_from_latin1(const uint8_t* input, size_t length) {
  return simdutf::utf8_length_from_latin1(reinterpret_cast<const char*>(input), length);
}

inline size_t simdutf_convert_latin1_to_utf8(
    const uint8_t* input, size_t length, uint8_t* utf8_output) {
  return simdutf::convert_latin1_to_utf8(
      reinterpret_cast<const char*>(input), length, reinterpret_cast<char*>(utf8_output));
}

// Converts as many characters of `input` as fit in `utf8_length` bytes. Returns the bytes
// written and sets `input_count` to the code units read.
inline size_t simdutf_convert_latin1_to_utf8_safe(const uint8_t* input,
    size_t length,
    uint8_t* utf8_output,
    size_t utf8_length,
    size_t& input_count) {
  auto result =
      simdutf::convert_latin1_to_utf8_safe_with_details(reinterpret_cast<const char*>(input),
          length, reinterpret_cast<char*>(utf8_output), utf8_length);
  input_count = result.input_count;
  return result.output_count;
}

// Does not validate `input`.
inline size_t simdutf_utf8_length_from_utf16(const uint16_t* input, size_t length) {
  return simdutf::utf8_length_from_utf16(reinterpret_cast<const char16_t*>(input), length);
}

// The UTF-8 length of `input` with each lone surrogate replaced by U+FFFD. Sets `has_surrogates`
// if `input` contains a surrogate, paired or not.
inline size_t simdutf_utf8_length_from_utf16_with_replacement(
    const uint16_t* input, size_t length, bool& has_surrogates) {
  auto result = simdutf::utf8_length_from_utf16_with_replacement(
      reinterpret_cast<const char16_t*>(input), length);
  has_surrogates = result.error == simdutf::SURROGATE;
  return result.count;
}

// Replaces each lone surrogate in `input` with U+FFFD. `output` may be `input`.
inline void simdutf_to_well_formed_utf16(const uint16_t* input, size_t length, uint16_t* output) {
  simdutf::to_well_formed_utf16(
      reinterpret_cast<const char16_t*>(input), length, reinterpret_cast<char16_t*>(output));
}

// Converts well-formed UTF-16.
inline size_t simdutf_convert_utf16_to_utf8(
    const uint16_t* input, size_t length, uint8_t* utf8_output) {
  return simdutf::convert_utf16_to_utf8(
      reinterpret_cast<const char16_t*>(input), length, reinterpret_cast<char*>(utf8_output));
}

// Converts `input`, or stops at its first lone surrogate and clears `ok`. Returns the bytes
// written when `ok`, and the position of the lone surrogate otherwise.
inline size_t simdutf_convert_utf16_to_utf8_with_errors(
    const uint16_t* input, size_t length, uint8_t* utf8_output, bool& ok) {
  auto result = simdutf::convert_utf16_to_utf8_with_errors(
      reinterpret_cast<const char16_t*>(input), length, reinterpret_cast<char*>(utf8_output));
  ok = result.error == simdutf::SUCCESS;
  return result.count;
}

// The outcomes of simdutf_convert_utf16_to_utf8_safe().
enum : uint8_t {
  SIMDUTF_SUCCESS = 0,
  SIMDUTF_OUTPUT_BUFFER_TOO_SMALL = 1,
  SIMDUTF_SURROGATE = 2,
  SIMDUTF_OTHER_ERROR = 3,
};

// Converts as many characters of `input` as fit in `utf8_length` bytes, stopping at a lone
// surrogate. Sets `input_count` and `output_count` to the code units read and bytes written.
inline uint8_t simdutf_convert_utf16_to_utf8_safe(const uint16_t* input,
    size_t length,
    uint8_t* utf8_output,
    size_t utf8_length,
    size_t& input_count,
    size_t& output_count) {
  auto result =
      simdutf::convert_utf16_to_utf8_safe_with_details(reinterpret_cast<const char16_t*>(input),
          length, reinterpret_cast<char*>(utf8_output), utf8_length);
  input_count = result.input_count;
  output_count = result.output_count;
  switch (result.error) {
    case simdutf::SUCCESS:
      return SIMDUTF_SUCCESS;
    case simdutf::OUTPUT_BUFFER_TOO_SMALL:
      return SIMDUTF_OUTPUT_BUFFER_TOO_SMALL;
    case simdutf::SURROGATE:
      return SIMDUTF_SURROGATE;
    default:
      return SIMDUTF_OTHER_ERROR;
  }
}

// Converts as many characters of `input` as fit in `utf8_length` bytes, replacing each lone
// surrogate with U+FFFD. Returns the bytes written and sets `input_count` to the code units read.
inline size_t simdutf_convert_utf16_to_utf8_with_replacement_safe(const uint16_t* input,
    size_t length,
    uint8_t* utf8_output,
    size_t utf8_length,
    size_t& input_count) {
  auto result =
      simdutf::convert_utf16_to_utf8_with_replacement_safe(reinterpret_cast<const char16_t*>(input),
          length, reinterpret_cast<char*>(utf8_output), utf8_length);
  input_count = result.input_count;
  return result.output_count;
}

}  // namespace workerd::rust::encoding
