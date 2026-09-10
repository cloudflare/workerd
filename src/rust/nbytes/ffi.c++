// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <nbytes.h>
#include <simdutf.h>

namespace workerd::rust::nbytes {

size_t base64DecodedSize(::rust::Slice<const uint8_t> input) {
  auto src = reinterpret_cast<const char*>(input.data());
  return ::nbytes::Base64DecodedSize(src, input.size());
}

size_t base64DecodeInto(::rust::Slice<uint8_t> output, ::rust::Slice<const uint8_t> input) {
  auto dest = reinterpret_cast<char*>(output.data());
  auto src = reinterpret_cast<const char*>(input.data());
  return ::nbytes::Base64Decode(dest, output.size(), src, input.size());
}

size_t base64EncodedSize(size_t inputSize, bool url) {
  return simdutf::base64_length_from_binary(
      inputSize, url ? simdutf::base64_url : simdutf::base64_default);
}

size_t base64EncodeInto(
    ::rust::Slice<uint8_t> output, ::rust::Slice<const uint8_t> input, bool url) {
  auto dest = reinterpret_cast<char*>(output.data());
  auto src = reinterpret_cast<const char*>(input.data());
  return simdutf::binary_to_base64(
      src, input.size(), dest, url ? simdutf::base64_url : simdutf::base64_default);
}

}  // namespace workerd::rust::nbytes
