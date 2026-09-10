// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <rust/cxx.h>

namespace workerd::rust::nbytes {

size_t base64DecodedSize(::rust::Slice<const uint8_t> input);
size_t base64DecodeInto(::rust::Slice<uint8_t> output, ::rust::Slice<const uint8_t> input);
size_t base64EncodedSize(size_t inputSize, bool url);
size_t base64EncodeInto(
    ::rust::Slice<uint8_t> output, ::rust::Slice<const uint8_t> input, bool url);

}  // namespace workerd::rust::nbytes
