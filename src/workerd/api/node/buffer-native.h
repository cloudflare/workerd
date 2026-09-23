// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

// `node-internal:buffer_native`: the library primitives behind the TypeScript
// implementation of `node-internal:buffer` (src/node/internal/buffer.ts). Each
// method wraps exactly one V8, simdutf, nbytes, kj, or i18n call made by
// BufferUtil (buffer.c++), with the same options, so both implementations reach
// the same library code. The TypeScript module validates arguments before
// calling in; the checks here only keep each primitive memory-safe on its own.
class BufferNative final: public jsg::Object {
 public:
  BufferNative() = default;
  BufferNative(jsg::Lock&, const jsg::Url&) {}

  // V8 strings
  double utf8Length(jsg::Lock& js, jsg::JsString string);
  double writeOneByte(jsg::Lock& js, jsg::JsString string, jsg::JsUint8Array dest, uint8_t flags);
  double writeUtf8(jsg::Lock& js, jsg::JsString string, jsg::JsUint8Array dest, uint8_t flags);
  double writeUtf16(jsg::Lock& js, jsg::JsString string, jsg::JsUint8Array dest, uint8_t flags);
  jsg::JsString newFromOneByte(jsg::Lock& js, jsg::JsUint8Array bytes);
  jsg::JsString newFromUtf8(jsg::Lock& js, jsg::JsUint8Array bytes);
  jsg::JsString newFromTwoByte(jsg::Lock& js, jsg::JsUint8Array bytes);

  // simdutf
  double simdutfMaximalBinaryLengthFromBase64(jsg::JsUint8Array input);
  double simdutfBase64ToBinary(jsg::JsUint8Array input, jsg::JsUint8Array output);
  double simdutfBase64LengthFromBinary(double length);
  double simdutfBase64UrlLengthFromBinary(double length);
  double simdutfBinaryToBase64(jsg::JsUint8Array input, jsg::JsUint8Array output);
  double simdutfBinaryToBase64Url(jsg::JsUint8Array input, jsg::JsUint8Array output);
  bool simdutfValidateAscii(jsg::JsUint8Array input);
  bool simdutfValidateUtf8(jsg::JsUint8Array input);

  // nbytes
  double nbytesBase64Decode(jsg::JsUint8Array output, jsg::JsUint8Array input);
  bool nbytesSwapBytes16(jsg::JsUint8Array data);
  bool nbytesSwapBytes32(jsg::JsUint8Array data);
  bool nbytesSwapBytes64(jsg::JsUint8Array data);

  // kj
  jsg::JsUint8Array kjEncodeHex(jsg::Lock& js, jsg::JsUint8Array input);

  // i18n
  jsg::JsUint8Array transcode(
      jsg::Lock& js, jsg::JsUint8Array source, uint8_t fromEncoding, uint8_t toEncoding);

  JSG_RESOURCE_TYPE(BufferNative) {
    JSG_STATIC_CONSTANT_NAMED(WRITE_NONE, static_cast<uint8_t>(jsg::JsString::WriteFlags::NONE));
    JSG_STATIC_CONSTANT_NAMED(
        WRITE_NULL_TERMINATION, static_cast<uint8_t>(jsg::JsString::WriteFlags::NULL_TERMINATION));
    JSG_STATIC_CONSTANT_NAMED(WRITE_REPLACE_INVALID_UTF8,
        static_cast<uint8_t>(jsg::JsString::WriteFlags::REPLACE_INVALID_UTF8));

    JSG_METHOD(utf8Length);
    JSG_METHOD(writeOneByte);
    JSG_METHOD(writeUtf8);
    JSG_METHOD(writeUtf16);
    JSG_METHOD(newFromOneByte);
    JSG_METHOD(newFromUtf8);
    JSG_METHOD(newFromTwoByte);

    JSG_METHOD(simdutfMaximalBinaryLengthFromBase64);
    JSG_METHOD(simdutfBase64ToBinary);
    JSG_METHOD(simdutfBase64LengthFromBinary);
    JSG_METHOD(simdutfBase64UrlLengthFromBinary);
    JSG_METHOD(simdutfBinaryToBase64);
    JSG_METHOD(simdutfBinaryToBase64Url);
    JSG_METHOD(simdutfValidateAscii);
    JSG_METHOD(simdutfValidateUtf8);

    JSG_METHOD(nbytesBase64Decode);
    JSG_METHOD(nbytesSwapBytes16);
    JSG_METHOD(nbytesSwapBytes32);
    JSG_METHOD(nbytesSwapBytes64);

    JSG_METHOD(kjEncodeHex);

    JSG_METHOD(transcode);
  }
};

#define EW_NODE_BUFFER_NATIVE_ISOLATE_TYPES api::node::BufferNative

}  // namespace workerd::api::node
