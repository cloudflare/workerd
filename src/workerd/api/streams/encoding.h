// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "readable.h"
#include "writable.h"
#include "transform.h"

namespace workerd::api {

class TextEncoderStream: public TransformStream {
public:
  using TransformStream::TransformStream;

  static jsg::Ref<TextEncoderStream> constructor(
      jsg::Lock& js,
      CompatibilityFlags::Reader flags);

  JSG_RESOURCE_TYPE(TextEncoderStream) {
    JSG_INHERIT(TransformStream);

    JSG_TS_OVERRIDE(extends TransformStream<string, Uint8Array>);
  }
};

class TextDecoderStream: public TransformStream {
public:
  using TransformStream::TransformStream;

  struct TextDecoderStreamInit {
    jsg::Optional<bool> fatal;

    JSG_STRUCT(fatal);
  };

  static jsg::Ref<TextDecoderStream> constructor(
      jsg::Lock& js,
      jsg::Optional<kj::String> label,
      jsg::Optional<TextDecoderStreamInit> options,
      CompatibilityFlags::Reader flags);

  JSG_RESOURCE_TYPE(TextDecoderStream) {
    JSG_INHERIT(TransformStream);

    JSG_TS_OVERRIDE(extends TransformStream<ArrayBuffer | ArrayBufferView, string>);
  }
};

}  // namespace workerd::api
