// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/jsg/async-context.h>
#include <kj/table.h>

namespace workerd::api::node {

class BufferUtil final: public jsg::Object {
  // Implements utilities in support of the Node.js Buffer
public:

  uint32_t byteLength(jsg::Lock& js, v8::Local<v8::String> str);

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

  kj::Array<kj::byte> concat(jsg::Lock& js,
                             kj::Array<kj::Array<kj::byte>> list,
                             uint32_t length);

  kj::Array<kj::byte> decodeString(jsg::Lock& js,
                                   v8::Local<v8::String> string,
                                   kj::String encoding);

  void fillImpl(jsg::Lock& js,
                kj::Array<kj::byte> buffer,
                kj::OneOf<v8::Local<v8::String>, jsg::BufferSource> value,
                uint32_t start,
                uint32_t end,
                jsg::Optional<kj::String> encoding);

  jsg::Optional<uint32_t> indexOf(jsg::Lock& js,
                                  kj::Array<kj::byte> buffer,
                                  kj::OneOf<v8::Local<v8::String>,
                                            jsg::BufferSource> value,
                                  jsg::Optional<int32_t> byteOffset,
                                  jsg::Optional<kj::String> encoding,
                                  bool findLast = false);

  void swap(jsg::Lock& js, kj::Array<kj::byte> buffer, int size);

  v8::Local<v8::String> toString(jsg::Lock& js,
                                 kj::Array<kj::byte> bytes,
                                 uint32_t start,
                                 uint32_t end,
                                 kj::String encoding);

  uint32_t write(jsg::Lock& js,
                 kj::Array<kj::byte> buffer,
                 v8::Local<v8::String> string,
                 uint32_t offset,
                 uint32_t length,
                 kj::String encoding);

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
  }
};

#define EW_NODE_BUFFER_ISOLATE_TYPES       \
    api::node::BufferUtil,                 \
    api::node::BufferUtil::CompareOptions

}  // namespace workerd::api::node
