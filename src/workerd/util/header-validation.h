// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <kj/string.h>

#include <cstdint>

// Platform-specific intrinsics headers
#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || defined(_M_IX86)
#include <emmintrin.h>
#endif

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace workerd::util {

// SIMD-accelerated validation for HTTP header values.
// Checks that the value contains no NULL (0x00), CR (0x0D), or LF (0x0A) characters.
// Returns true if the value is valid, false otherwise.
//
// This function automatically selects the best implementation based on available CPU features:
// - AVX2 for modern x86/x64 (32 bytes per iteration)
// - SSE2 for older x86/x64 (16 bytes per iteration)
// - NEON for ARM/ARM64 (16 bytes per iteration)
// - Scalar fallback for unsupported platforms or short strings

#if defined(__AVX2__)
// AVX2 implementation: Process 32 bytes at a time
inline bool isValidHeaderValueSIMD_AVX2(const char* ptr, size_t len) {
  const __m256i zero = _mm256_setzero_si256();
  const __m256i cr = _mm256_set1_epi8('\r');
  const __m256i lf = _mm256_set1_epi8('\n');

  while (len >= 32) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr));

    // Compare against invalid characters
    __m256i nulls = _mm256_cmpeq_epi8(chunk, zero);
    __m256i crs = _mm256_cmpeq_epi8(chunk, cr);
    __m256i lfs = _mm256_cmpeq_epi8(chunk, lf);

    // Combine: any match means invalid
    __m256i invalid = _mm256_or_si256(_mm256_or_si256(nulls, crs), lfs);

    // Check if any byte matched (non-zero mask means invalid char found)
    uint32_t mask = _mm256_movemask_epi8(invalid);
    if (__builtin_expect(mask != 0, 0)) {
      return false;
    }

    ptr += 32;
    len -= 32;
  }

  // Process remaining 16-31 bytes with SSE2 if available
  if (len >= 16) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
    __m128i zero_sse = _mm_setzero_si128();
    __m128i cr_sse = _mm_set1_epi8('\r');
    __m128i lf_sse = _mm_set1_epi8('\n');

    __m128i nulls = _mm_cmpeq_epi8(chunk, zero_sse);
    __m128i crs = _mm_cmpeq_epi8(chunk, cr_sse);
    __m128i lfs = _mm_cmpeq_epi8(chunk, lf_sse);

    __m128i invalid = _mm_or_si128(_mm_or_si128(nulls, crs), lfs);

    int mask = _mm_movemask_epi8(invalid);
    if (__builtin_expect(mask != 0, 0)) {
      return false;
    }

    ptr += 16;
    len -= 16;
  }

  // Scalar fallback for remaining 0-15 bytes
  for (size_t i = 0; i < len; ++i) {
    char c = ptr[i];
    if (c == '\0' || c == '\r' || c == '\n') {
      return false;
    }
  }

  return true;
}
#endif  // __AVX2__

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || defined(_M_IX86)
// SSE2 implementation: Process 16 bytes at a time
inline bool isValidHeaderValueSIMD_SSE2(const char* ptr, size_t len) {
  const __m128i zero = _mm_setzero_si128();
  const __m128i cr = _mm_set1_epi8('\r');
  const __m128i lf = _mm_set1_epi8('\n');

  while (len >= 16) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));

    // Compare against invalid characters
    __m128i nulls = _mm_cmpeq_epi8(chunk, zero);
    __m128i crs = _mm_cmpeq_epi8(chunk, cr);
    __m128i lfs = _mm_cmpeq_epi8(chunk, lf);

    // Combine: any match means invalid
    __m128i invalid = _mm_or_si128(_mm_or_si128(nulls, crs), lfs);

    // Check if any byte matched (non-zero mask means invalid char found)
    int mask = _mm_movemask_epi8(invalid);
    if (__builtin_expect(mask != 0, 0)) {
      return false;
    }

    ptr += 16;
    len -= 16;
  }

  // Scalar fallback for remaining 0-15 bytes
  for (size_t i = 0; i < len; ++i) {
    char c = ptr[i];
    if (c == '\0' || c == '\r' || c == '\n') {
      return false;
    }
  }

  return true;
}
#endif  // SSE2

#if defined(__ARM_NEON) || defined(__aarch64__)
// ARM NEON implementation: Process 16 bytes at a time
inline bool isValidHeaderValueSIMD_NEON(const char* ptr, size_t len) {
  const uint8x16_t zero = vdupq_n_u8(0);
  const uint8x16_t cr = vdupq_n_u8('\r');
  const uint8x16_t lf = vdupq_n_u8('\n');

  while (len >= 16) {
    uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(ptr));

    // Compare operations
    uint8x16_t is_null = vceqq_u8(chunk, zero);
    uint8x16_t is_cr = vceqq_u8(chunk, cr);
    uint8x16_t is_lf = vceqq_u8(chunk, lf);

    // Combine
    uint8x16_t invalid = vorrq_u8(vorrq_u8(is_null, is_cr), is_lf);

    // Check if any lane is set (vmaxvq returns maximum value across vector)
    if (__builtin_expect(vmaxvq_u8(invalid) != 0, 0)) {
      return false;
    }

    ptr += 16;
    len -= 16;
  }

  // Scalar fallback for remaining 0-15 bytes
  for (size_t i = 0; i < len; ++i) {
    char c = ptr[i];
    if (c == '\0' || c == '\r' || c == '\n') {
      return false;
    }
  }

  return true;
}
#endif  // ARM_NEON

// Scalar fallback implementation for platforms without SIMD support
inline bool isValidHeaderValueScalar(const char* ptr, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    char c = ptr[i];
    if (c == '\0' || c == '\r' || c == '\n') {
      return false;
    }
  }
  return true;
}

// Main entry point: Automatically dispatches to the best available implementation
inline bool isValidHeaderValue(kj::StringPtr value) {
  const char* ptr = value.begin();
  size_t len = value.size();

  // Empty strings are valid
  if (len == 0) return true;

    // Dispatch to best available implementation
#if defined(__AVX2__)
  return isValidHeaderValueSIMD_AVX2(ptr, len);
#elif defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
  return isValidHeaderValueSIMD_SSE2(ptr, len);
#elif defined(__ARM_NEON) || defined(__aarch64__)
  return isValidHeaderValueSIMD_NEON(ptr, len);
#else
  return isValidHeaderValueScalar(ptr, len);
#endif
}

// Bitfield flags for HTTP character lookup table
constexpr uint8_t HTTP_TOKEN_CHAR = 0x01;  // Valid HTTP token character
constexpr uint8_t HTTP_WHITESPACE = 0x02;  // HTTP whitespace (tab, space, CR, LF)

// Fast lookup table for HTTP character validation using bitfields (RFC 2616).
// Combines checks for: token chars and HTTP whitespace.
// Valid token chars are: !#$%&'*+-.0-9A-Z^_`a-z|~
// (i.e., any CHAR except CTLs or separators)
// HTTP whitespace chars are: tab, space, CR, LF
static constexpr uint8_t HTTP_TOKEN_CHAR_TABLE[] = {
  // Control characters 0x00-0x1F and 0x7F are invalid
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x00-0x07
  0, 2, 2, 0, 0, 2, 0, 0,  // 0x08-0x0F (tab=2, LF=2, CR=2)
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x10-0x17
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x18-0x1F
  2, 1, 0, 1, 1, 1, 1, 1,  // 0x20-0x27: SP!"#$%&'
  0, 0, 1, 1, 0, 1, 1, 0,  // 0x28-0x2F: ()*+,-./
  1, 1, 1, 1, 1, 1, 1, 1,  // 0x30-0x37: 01234567
  1, 1, 0, 0, 0, 0, 0, 0,  // 0x38-0x3F: 89:;<=>?
  0, 1, 1, 1, 1, 1, 1, 1,  // 0x40-0x47: @ABCDEFG
  1, 1, 1, 1, 1, 1, 1, 1,  // 0x48-0x4F: HIJKLMNO
  1, 1, 1, 1, 1, 1, 1, 1,  // 0x50-0x57: PQRSTUVW
  1, 1, 1, 0, 0, 0, 1, 1,  // 0x58-0x5F: XYZ[\]^_
  1, 1, 1, 1, 1, 1, 1, 1,  // 0x60-0x67: `abcdefg
  1, 1, 1, 1, 1, 1, 1, 1,  // 0x68-0x6F: hijklmno
  1, 1, 1, 1, 1, 1, 1, 1,  // 0x70-0x77: pqrstuvw
  1, 1, 1, 0, 1, 0, 1, 0,  // 0x78-0x7F: xyz{|}~DEL
  // Extended ASCII 0x80-0xFF are all invalid per RFC 2616
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x80-0x87
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x88-0x8F
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x90-0x97
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x98-0x9F
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xA0-0xA7
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xA8-0xAF
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xB0-0xB7
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xB8-0xBF
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xC0-0xC7
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xC8-0xCF
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xD0-0xD7
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xD8-0xDF
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xE0-0xE7
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xE8-0xEF
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xF0-0xF7
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xF8-0xFF
};

inline constexpr bool isHttpWhitespace(char c) {
  return HTTP_TOKEN_CHAR_TABLE[static_cast<uint8_t>(c)] & HTTP_WHITESPACE;
}
static_assert(isHttpWhitespace(' '));
static_assert(!isHttpWhitespace('A'));
inline constexpr bool isHttpTokenChar(char c) {
  return HTTP_TOKEN_CHAR_TABLE[static_cast<uint8_t>(c)] & HTTP_TOKEN_CHAR;
}
static_assert(isHttpTokenChar('A'));
static_assert(!isHttpTokenChar(' '));
}  // namespace workerd::util
