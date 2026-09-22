// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "buffer-ffi.h"

#include <workerd/api/node/i18n.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/util.h>
#include <workerd/rust/jsg/ffi-inl.h>

#include <nbytes.h>
#include <simdutf.h>

#include <kj/encoding.h>

namespace workerd::rust::api {

namespace {
const char* asChars(::rust::Slice<const uint8_t> input) {
  return reinterpret_cast<const char*>(input.data());
}

char* asChars(::rust::Slice<uint8_t> output) {
  return reinterpret_cast<char*>(output.data());
}
}  // namespace

size_t simdutfMaximalBinaryLengthFromBase64(::rust::Slice<const uint8_t> input) {
  return simdutf::maximal_binary_length_from_base64(asChars(input), input.size());
}

size_t simdutfBase64ToBinary(::rust::Slice<const uint8_t> input, ::rust::Slice<uint8_t> output) {
  // As in decodeStringImpl in buffer.c++, the error code is ignored and the count is used. The
  // Rust caller guarantees output is at least maximal_binary_length_from_base64(input) bytes.
  auto result = simdutf::base64_to_binary(
      asChars(input), input.size(), asChars(output), simdutf::base64_default_or_url_accept_garbage);
  return result.count;
}

size_t simdutfBase64LengthFromBinary(size_t length) {
  return simdutf::base64_length_from_binary(length);
}

size_t simdutfBase64UrlLengthFromBinary(size_t length) {
  return simdutf::base64_length_from_binary(length, simdutf::base64_url);
}

size_t simdutfBinaryToBase64(::rust::Slice<const uint8_t> input, ::rust::Slice<uint8_t> output) {
  // The Rust caller guarantees output is at least base64_length_from_binary(input) bytes.
  return simdutf::binary_to_base64(asChars(input), input.size(), asChars(output));
}

size_t simdutfBinaryToBase64Url(::rust::Slice<const uint8_t> input, ::rust::Slice<uint8_t> output) {
  // The Rust caller guarantees output is at least base64_length_from_binary(input) bytes.
  return simdutf::binary_to_base64(
      asChars(input), input.size(), asChars(output), simdutf::base64_url);
}

bool simdutfValidateAscii(::rust::Slice<const uint8_t> input) {
  return simdutf::validate_ascii(asChars(input), input.size());
}

bool simdutfValidateUtf8(::rust::Slice<const uint8_t> input) {
  return simdutf::validate_utf8(asChars(input), input.size());
}

size_t nbytesBase64Decode(::rust::Slice<uint8_t> output, ::rust::Slice<const uint8_t> input) {
  return ::nbytes::Base64Decode(asChars(output), output.size(), asChars(input), input.size());
}

bool nbytesSwapBytes16(::rust::Slice<uint8_t> data) {
  return ::nbytes::SwapBytes16(asChars(data), data.size());
}

bool nbytesSwapBytes32(::rust::Slice<uint8_t> data) {
  return ::nbytes::SwapBytes32(asChars(data), data.size());
}

bool nbytesSwapBytes64(::rust::Slice<uint8_t> data) {
  return ::nbytes::SwapBytes64(asChars(data), data.size());
}

void kjEncodeHex(::rust::Slice<const uint8_t> input, ::rust::Slice<uint8_t> output) {
  // The Rust caller guarantees output is exactly twice the size of input.
  auto hex = kj::encodeHex(kj::arrayPtr(input.data(), input.size()));
  kj::arrayPtr(output.data(), output.size()).copyFrom(hex.asBytes());
}

::workerd::rust::jsg::Local i18nTranscode(::workerd::rust::jsg::Isolate* isolate,
    ::rust::Slice<const uint8_t> source,
    uint8_t fromEncoding,
    uint8_t toEncoding) {
  auto& js = ::workerd::jsg::Lock::from(isolate);
  JSG_TRY(js) {
    // i18n::transcode only reads from the source.
    auto bytes = kj::arrayPtr(const_cast<kj::byte*>(source.data()), source.size());
    v8::Local<v8::Value> result = ::workerd::api::node::i18n::transcode(js, bytes,
        static_cast<::workerd::api::node::Encoding>(fromEncoding),
        static_cast<::workerd::api::node::Encoding>(toEncoding));
    return ::workerd::rust::jsg::to_ffi(kj::mv(result));
  }
  JSG_CATCH(error) {
    kj::throwFatalException(::workerd::jsg::createTunneledException(isolate, error.getHandle(js)));
  };
}

}  // namespace workerd::rust::api
