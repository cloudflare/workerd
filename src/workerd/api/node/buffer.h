// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include "i18n.h"

#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

// Implements utilities in support of the Node.js Buffer
class BufferUtil final: public jsg::Object {
 public:
  BufferUtil() = default;
  BufferUtil(jsg::Lock&, const jsg::Url&) {}

  uint32_t byteLength(jsg::Lock& js, jsg::JsString str);

  struct CompareOptions {
    jsg::Optional<uint32_t> aStart;
    jsg::Optional<uint32_t> aEnd;
    jsg::Optional<uint32_t> bStart;
    jsg::Optional<uint32_t> bEnd;

    JSG_STRUCT(aStart, aEnd, bStart, bEnd);
  };

  int compare(jsg::Lock& js,
      kj::Array<kj::byte> one,
      kj::Array<kj::byte> two,
      jsg::Optional<CompareOptions> maybeOptions);

  jsg::BufferSource concat(jsg::Lock& js, kj::Array<kj::Array<kj::byte>> list, uint32_t length);

  jsg::BufferSource decodeString(jsg::Lock& js, jsg::JsString string, EncodingValue encoding);

  void fillImpl(jsg::Lock& js,
      kj::Array<kj::byte> buffer,
      kj::OneOf<jsg::JsString, jsg::BufferSource> value,
      uint32_t start,
      uint32_t end,
      jsg::Optional<EncodingValue> encoding);

  jsg::Optional<uint32_t> indexOf(jsg::Lock& js,
      kj::Array<kj::byte> buffer,
      kj::OneOf<jsg::JsString, jsg::BufferSource> value,
      int32_t byteOffset,
      EncodingValue encoding,
      bool isForward);

  void swap(jsg::Lock& js, kj::Array<kj::byte> buffer, int size);

  jsg::JsString toString(jsg::Lock& js,
      kj::Array<kj::byte> bytes,
      uint32_t start,
      uint32_t end,
      EncodingValue encoding);

  uint32_t write(jsg::Lock& js,
      kj::Array<kj::byte> buffer,
      jsg::JsString string,
      uint32_t offset,
      uint32_t length,
      EncodingValue encoding);

  enum NativeDecoderFields {
    kIncompleteCharactersStart = 0,
    kIncompleteCharactersEnd = 4,
    kMissingBytes = 4,
    kBufferedBytes = 5,
    kEncoding = 6,
    kSize = 7,
  };

  jsg::JsString decode(jsg::Lock& js, kj::Array<kj::byte> bytes, kj::Array<kj::byte> state);
  jsg::JsString flush(jsg::Lock& js, kj::Array<kj::byte> state);
  bool isAscii(kj::Array<kj::byte> bytes);
  bool isUtf8(kj::Array<kj::byte> bytes);
  jsg::BufferSource transcode(jsg::Lock& js,
      kj::Array<kj::byte> source,
      EncodingValue rawFromEncoding,
      EncodingValue rawToEncoding);

  JSG_RESOURCE_TYPE(BufferUtil) {
    JSG_METHOD(byteLength);
    JSG_METHOD(compare);
    JSG_METHOD(concat);
    JSG_METHOD(decodeString);
    JSG_METHOD(fillImpl);
    JSG_METHOD(indexOf);
    JSG_METHOD(swap);
    JSG_METHOD(toString);
    JSG_METHOD(write);
    JSG_METHOD(isAscii);
    JSG_METHOD(isUtf8);
    JSG_METHOD(transcode);

    // For StringDecoder
    JSG_METHOD(decode);
    JSG_METHOD(flush);

    JSG_STATIC_CONSTANT_NAMED(ASCII, static_cast<EncodingValue>(Encoding::ASCII));
    JSG_STATIC_CONSTANT_NAMED(LATIN1, static_cast<EncodingValue>(Encoding::LATIN1));
    JSG_STATIC_CONSTANT_NAMED(UTF8, static_cast<EncodingValue>(Encoding::UTF8));
    JSG_STATIC_CONSTANT_NAMED(UTF16LE, static_cast<EncodingValue>(Encoding::UTF16LE));
    JSG_STATIC_CONSTANT_NAMED(BASE64, static_cast<EncodingValue>(Encoding::BASE64));
    JSG_STATIC_CONSTANT_NAMED(BASE64URL, static_cast<EncodingValue>(Encoding::BASE64URL));
    JSG_STATIC_CONSTANT_NAMED(HEX, static_cast<EncodingValue>(Encoding::HEX));
  }
};

#define EW_NODE_BUFFER_ISOLATE_TYPES api::node::BufferUtil, api::node::BufferUtil::CompareOptions

}  // namespace workerd::api::node
