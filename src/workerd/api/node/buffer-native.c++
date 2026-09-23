// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "buffer-native.h"

#include "i18n.h"

#include <nbytes.h>
#include <simdutf.h>

#include <kj/encoding.h>

namespace workerd::api::node {

namespace {

jsg::JsString::WriteFlags toWriteFlags(uint8_t flags) {
  constexpr uint8_t kAll =
      jsg::JsString::WriteFlags::NULL_TERMINATION | jsg::JsString::WriteFlags::REPLACE_INVALID_UTF8;
  JSG_REQUIRE((flags & ~kAll) == 0, TypeError, "Invalid write flags");
  return static_cast<jsg::JsString::WriteFlags>(flags);
}

// Write flags for the primitives that pass V8 an exact element count. V8
// writes a null terminator after that count, which would run past `dest`, so
// null termination is only accepted by writeUtf8.
jsg::JsString::WriteFlags toCountedWriteFlags(uint8_t flags) {
  auto result = toWriteFlags(flags);
  JSG_REQUIRE((result & jsg::JsString::WriteFlags::NULL_TERMINATION) == 0, TypeError,
      "Null termination is only supported by writeUtf8");
  return result;
}

const char* asChars(jsg::JsUint8Array& array) {
  return array.asArrayPtr().asChars().begin();
}

}  // namespace

// jsg::JsString::utf8Length()
double BufferNative::utf8Length(jsg::Lock& js, jsg::JsString string) {
  return string.utf8Length(js);
}

// jsg::JsString::writeInto(js, kj::ArrayPtr<kj::byte>, flags): writes the low
// byte of each UTF-16 code unit. Returns the number of bytes written.
double BufferNative::writeOneByte(
    jsg::Lock& js, jsg::JsString string, jsg::JsUint8Array dest, uint8_t flags) {
  return string.writeInto(js, dest.asArrayPtr(), toCountedWriteFlags(flags)).written;
}

// jsg::JsString::writeInto(js, kj::ArrayPtr<char>, flags): writes UTF-8.
// Returns the number of bytes written.
double BufferNative::writeUtf8(
    jsg::Lock& js, jsg::JsString string, jsg::JsUint8Array dest, uint8_t flags) {
  return string.writeInto(js, dest.asArrayPtr().asChars(), toWriteFlags(flags)).written;
}

// jsg::JsString::writeInto(js, kj::ArrayPtr<uint16_t>, flags) over `dest`
// reinterpreted as native-endian uint16_ts (the last byte of an odd-length
// `dest` is not written). Returns the number of code units written.
double BufferNative::writeUtf16(
    jsg::Lock& js, jsg::JsString string, jsg::JsUint8Array dest, uint8_t flags) {
  auto options = toCountedWriteFlags(flags);
  auto bytes = dest.asArrayPtr();
#if __has_feature(undefined_behavior_sanitizer)
  // UBSan warns about unaligned writes, which are hard to avoid if dest is
  // unaligned. Write through an aligned buffer instead, as in buffer.c++.
  auto tmpBuf = kj::heapArray<uint16_t>(bytes.size() / sizeof(uint16_t));
  auto result = string.writeInto(js, tmpBuf, options);
  kj::ArrayPtr<uint16_t> buf(reinterpret_cast<uint16_t*>(bytes.begin()), result.written);
  buf.copyFrom(tmpBuf.first(result.written));
#else
  kj::ArrayPtr<uint16_t> buf(
      reinterpret_cast<uint16_t*>(bytes.begin()), bytes.size() / sizeof(uint16_t));
  auto result = string.writeInto(js, buf, options);
#endif
  return result.written;
}

// jsg::Lock::str(kj::ArrayPtr<const kj::byte>)
jsg::JsString BufferNative::newFromOneByte(jsg::Lock& js, jsg::JsUint8Array bytes) {
  return js.str(bytes.asArrayPtr().asConst());
}

// jsg::Lock::str(kj::ArrayPtr<const char>)
jsg::JsString BufferNative::newFromUtf8(jsg::Lock& js, jsg::JsUint8Array bytes) {
  return js.str(bytes.asArrayPtr().asChars().asConst());
}

// jsg::Lock::str(kj::ArrayPtr<const uint16_t>) over `bytes` reinterpreted as
// native-endian uint16_ts (a trailing odd byte is ignored). Copies to an
// aligned buffer first, as BufferUtil's toString does.
jsg::JsString BufferNative::newFromTwoByte(jsg::Lock& js, jsg::JsUint8Array bytes) {
  auto slice = bytes.asArrayPtr();
  kj::ArrayPtr<uint16_t> view(reinterpret_cast<uint16_t*>(slice.begin()), slice.size() / 2);
  KJ_STACK_ARRAY(uint16_t, data, view.size(), 1024, 4096);
  data.copyFrom(view);
  return js.str(data.asConst());
}

// simdutf::maximal_binary_length_from_base64
double BufferNative::simdutfMaximalBinaryLengthFromBase64(jsg::JsUint8Array input) {
  return simdutf::maximal_binary_length_from_base64(asChars(input), input.size());
}

// simdutf::base64_to_binary with base64_default_or_url_accept_garbage. Returns
// the number of bytes written; the error code is ignored, as in buffer.c++.
double BufferNative::simdutfBase64ToBinary(jsg::JsUint8Array input, jsg::JsUint8Array output) {
  JSG_REQUIRE(
      output.size() >= simdutf::maximal_binary_length_from_base64(asChars(input), input.size()),
      RangeError, "Output buffer is too small");
  auto result = simdutf::base64_to_binary(asChars(input), input.size(),
      output.asArrayPtr().asChars().begin(), simdutf::base64_default_or_url_accept_garbage);
  return result.count;
}

// simdutf::base64_length_from_binary with base64_default
double BufferNative::simdutfBase64LengthFromBinary(double length) {
  return simdutf::base64_length_from_binary(static_cast<size_t>(length));
}

// simdutf::base64_length_from_binary with base64_url
double BufferNative::simdutfBase64UrlLengthFromBinary(double length) {
  return simdutf::base64_length_from_binary(static_cast<size_t>(length), simdutf::base64_url);
}

// simdutf::binary_to_base64 with base64_default
double BufferNative::simdutfBinaryToBase64(jsg::JsUint8Array input, jsg::JsUint8Array output) {
  JSG_REQUIRE(output.size() >= simdutf::base64_length_from_binary(input.size()), RangeError,
      "Output buffer is too small");
  return simdutf::binary_to_base64(
      asChars(input), input.size(), output.asArrayPtr().asChars().begin());
}

// simdutf::binary_to_base64 with base64_url
double BufferNative::simdutfBinaryToBase64Url(jsg::JsUint8Array input, jsg::JsUint8Array output) {
  JSG_REQUIRE(
      output.size() >= simdutf::base64_length_from_binary(input.size(), simdutf::base64_url),
      RangeError, "Output buffer is too small");
  return simdutf::binary_to_base64(
      asChars(input), input.size(), output.asArrayPtr().asChars().begin(), simdutf::base64_url);
}

// simdutf::validate_ascii
bool BufferNative::simdutfValidateAscii(jsg::JsUint8Array input) {
  return simdutf::validate_ascii(asChars(input), input.size());
}

// simdutf::validate_utf8
bool BufferNative::simdutfValidateUtf8(jsg::JsUint8Array input) {
  return simdutf::validate_utf8(asChars(input), input.size());
}

// nbytes::Base64Decode. Returns the number of bytes written, which is at most
// output.length.
double BufferNative::nbytesBase64Decode(jsg::JsUint8Array output, jsg::JsUint8Array input) {
  return nbytes::Base64Decode(
      output.asArrayPtr().asChars().begin(), output.size(), asChars(input), input.size());
}

// nbytes::SwapBytes16
bool BufferNative::nbytesSwapBytes16(jsg::JsUint8Array data) {
  return nbytes::SwapBytes16(data.asArrayPtr().asChars().begin(), data.size());
}

// nbytes::SwapBytes32
bool BufferNative::nbytesSwapBytes32(jsg::JsUint8Array data) {
  return nbytes::SwapBytes32(data.asArrayPtr().asChars().begin(), data.size());
}

// nbytes::SwapBytes64
bool BufferNative::nbytesSwapBytes64(jsg::JsUint8Array data) {
  return nbytes::SwapBytes64(data.asArrayPtr().asChars().begin(), data.size());
}

// kj::encodeHex. Returns the lowercase hex digits as bytes.
jsg::JsUint8Array BufferNative::kjEncodeHex(jsg::Lock& js, jsg::JsUint8Array input) {
  auto hex = kj::encodeHex(input.asArrayPtr());
  auto result = jsg::JsUint8Array::create(js, hex.size());
  result.asArrayPtr().copyFrom(hex.asBytes());
  return result;
}

// i18n::transcode
jsg::JsUint8Array BufferNative::transcode(
    jsg::Lock& js, jsg::JsUint8Array source, uint8_t fromEncoding, uint8_t toEncoding) {
  auto from = static_cast<Encoding>(fromEncoding);
  auto to = static_cast<Encoding>(toEncoding);
  JSG_REQUIRE(i18n::canBeTranscoded(from) && i18n::canBeTranscoded(to), Error,
      "Unable to transcode buffer due to unsupported encoding");
  return i18n::transcode(js, source.asArrayPtr(), from, to);
}

}  // namespace workerd::api::node
