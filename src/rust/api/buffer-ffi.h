// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

// C++ library entry points used by `node-internal:buffer_native` (buffer_native.rs). Each
// function forwards to exactly one simdutf, nbytes, KJ, or workerd call made by
// src/workerd/api/node/buffer.c++, with the same options, so the TypeScript implementation
// of `node-internal:buffer` reaches the same code as the C++ one.

#include <workerd/rust/jsg/ffi.h>

#include <rust/cxx.h>

#include <cstddef>
#include <cstdint>

namespace workerd::rust::api {

// simdutf
size_t simdutfMaximalBinaryLengthFromBase64(::rust::Slice<const uint8_t> input);
size_t simdutfBase64ToBinary(::rust::Slice<const uint8_t> input, ::rust::Slice<uint8_t> output);
size_t simdutfBase64LengthFromBinary(size_t length);
size_t simdutfBase64UrlLengthFromBinary(size_t length);
size_t simdutfBinaryToBase64(::rust::Slice<const uint8_t> input, ::rust::Slice<uint8_t> output);
size_t simdutfBinaryToBase64Url(::rust::Slice<const uint8_t> input, ::rust::Slice<uint8_t> output);
bool simdutfValidateAscii(::rust::Slice<const uint8_t> input);
bool simdutfValidateUtf8(::rust::Slice<const uint8_t> input);

// nbytes
size_t nbytesBase64Decode(::rust::Slice<uint8_t> output, ::rust::Slice<const uint8_t> input);
bool nbytesSwapBytes16(::rust::Slice<uint8_t> data);
bool nbytesSwapBytes32(::rust::Slice<uint8_t> data);
bool nbytesSwapBytes64(::rust::Slice<uint8_t> data);

// kj
void kjEncodeHex(::rust::Slice<const uint8_t> input, ::rust::Slice<uint8_t> output);

// workerd::api::node::i18n
//
// Returns a `Uint8Array`. Throws a tunneled `kj::Exception` on failure, which the Rust caller
// receives as an `Err` and converts back into the equivalent JS error.
::workerd::rust::jsg::Local i18nTranscode(::workerd::rust::jsg::Isolate* isolate,
    ::rust::Slice<const uint8_t> source,
    uint8_t fromEncoding,
    uint8_t toEncoding);

}  // namespace workerd::rust::api
