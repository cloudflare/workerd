// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

// `node-internal:buffer_native`: the library calls behind the TypeScript
// implementation of `node-internal:buffer` (src/node/internal/buffer.ts). Each
// method makes the V8, libc, simdutf, nbytes, kj, i18n, or string search
// (buffer-string-search.h) calls that BufferUtil
// (buffer.c++) makes at the same point, with the same options, so both
// implementations reach the same library code.
//
// Where BufferUtil operates on a slice of a buffer, the method takes the buffer
// with a `[start, end)` range, since a JavaScript view of the slice would cost
// an allocation that a kj::ArrayPtr slice does not. Where BufferUtil runs a
// sequence of library calls over a stack buffer, the method runs the whole
// sequence, since the stack buffer has no cheap JavaScript equivalent.
//
// The TypeScript module validates arguments before calling in; the checks here
// only keep each method memory-safe on its own.
class BufferNative final: public jsg::Object {
 public:
  BufferNative() = default;
  BufferNative(jsg::Lock&, const jsg::Url&) {}

  // V8 strings
  double utf8Length(jsg::Lock& js, jsg::JsString string);
  double writeOneByte(jsg::Lock& js,
      jsg::JsString string,
      jsg::JsUint8Array dest,
      double start,
      double end,
      uint8_t flags);
  double writeUtf8(jsg::Lock& js,
      jsg::JsString string,
      jsg::JsUint8Array dest,
      double start,
      double end,
      uint8_t flags);
  double writeUtf16(jsg::Lock& js,
      jsg::JsString string,
      jsg::JsUint8Array dest,
      double start,
      double end,
      uint8_t flags);
  jsg::JsString newFromOneByte(jsg::Lock& js, jsg::JsUint8Array bytes, double start, double end);
  jsg::JsString newFromUtf8(jsg::Lock& js, jsg::JsUint8Array bytes, double start, double end);
  jsg::JsString newFromTwoByte(jsg::Lock& js, jsg::JsUint8Array bytes, double start, double end);

  // String search (buffer-string-search.h)
  double searchOneByte(
      jsg::JsUint8Array haystack, jsg::JsUint8Array needle, double startIndex, bool isForward);
  double searchTwoByte(jsg::JsUint8Array haystack,
      double haystackLength,
      jsg::JsUint8Array needle,
      double needleLength,
      double startIndex,
      bool isForward);

  // libc
  int memcmp(jsg::JsUint8Array one,
      double oneStart,
      jsg::JsUint8Array two,
      double twoStart,
      double length);

  // simdutf
  jsg::JsUint8Array decodeBase64(jsg::Lock& js, jsg::JsString string);
  jsg::JsString encodeBase64(jsg::Lock& js, jsg::JsUint8Array bytes, double start, double end);
  jsg::JsString encodeBase64Url(jsg::Lock& js, jsg::JsUint8Array bytes, double start, double end);
  bool simdutfValidateAscii(jsg::JsUint8Array input);
  bool simdutfValidateUtf8(jsg::JsUint8Array input);

  // nbytes
  double nbytesBase64Decode(
      jsg::JsUint8Array output, double start, double end, jsg::JsUint8Array input);
  bool nbytesSwapBytes16(jsg::JsUint8Array data);
  bool nbytesSwapBytes32(jsg::JsUint8Array data);
  bool nbytesSwapBytes64(jsg::JsUint8Array data);

  // kj
  jsg::JsString encodeHex(jsg::Lock& js, jsg::JsUint8Array bytes, double start, double end);

  // i18n
  jsg::JsUint8Array transcode(
      jsg::Lock& js, jsg::JsUint8Array source, uint8_t fromEncoding, uint8_t toEncoding);

  JSG_RESOURCE_TYPE(BufferNative) {
    JSG_STATIC_CONSTANT_NAMED(WRITE_NONE, static_cast<uint8_t>(jsg::JsString::WriteFlags::NONE));
    JSG_STATIC_CONSTANT_NAMED(WRITE_REPLACE_INVALID_UTF8,
        static_cast<uint8_t>(jsg::JsString::WriteFlags::REPLACE_INVALID_UTF8));

    JSG_METHOD(utf8Length);
    JSG_METHOD(writeOneByte);
    JSG_METHOD(writeUtf8);
    JSG_METHOD(writeUtf16);
    JSG_METHOD(newFromOneByte);
    JSG_METHOD(newFromUtf8);
    JSG_METHOD(newFromTwoByte);

    JSG_METHOD(searchOneByte);
    JSG_METHOD(searchTwoByte);

    JSG_METHOD(memcmp);

    JSG_METHOD(decodeBase64);
    JSG_METHOD(encodeBase64);
    JSG_METHOD(encodeBase64Url);
    JSG_METHOD(simdutfValidateAscii);
    JSG_METHOD(simdutfValidateUtf8);

    JSG_METHOD(nbytesBase64Decode);
    JSG_METHOD(nbytesSwapBytes16);
    JSG_METHOD(nbytesSwapBytes32);
    JSG_METHOD(nbytesSwapBytes64);

    JSG_METHOD(encodeHex);

    JSG_METHOD(transcode);
  }
};

#define EW_NODE_BUFFER_NATIVE_ISOLATE_TYPES api::node::BufferNative

}  // namespace workerd::api::node
