// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "readable.h"
#include "transform.h"
#include "writable.h"

#include <workerd/api/encoding.h>

namespace workerd::api {

class TextEncoderStream: public TransformStream {
public:
  using TransformStream::TransformStream;

  static jsg::Ref<TextEncoderStream> constructor(jsg::Lock& js);

  kj::StringPtr getEncoding() {
    return "utf-8"_kj;
  }

  JSG_RESOURCE_TYPE(TextEncoderStream) {
    JSG_INHERIT(TransformStream);

    JSG_READONLY_PROTOTYPE_PROPERTY(encoding, getEncoding);

    JSG_TS_OVERRIDE(extends TransformStream<string, Uint8Array>);
  }
};

class TextDecoderStream: public TransformStream {
public:
  struct TextDecoderStreamInit {
    jsg::Optional<bool> fatal;
    jsg::Optional<bool> ignoreBOM;

    JSG_STRUCT(fatal, ignoreBOM);
  };

  TextDecoderStream(jsg::Ref<TextDecoder> decoder,
      jsg::Ref<ReadableStream> readable,
      jsg::Ref<WritableStream> writable);

  static jsg::Ref<TextDecoderStream> constructor(
      jsg::Lock& js, jsg::Optional<kj::String> label, jsg::Optional<TextDecoderStreamInit> options);

  kj::StringPtr getEncoding();
  bool getFatal();
  bool getIgnoreBOM();

  JSG_RESOURCE_TYPE(TextDecoderStream) {
    JSG_INHERIT(TransformStream);
    JSG_READONLY_PROTOTYPE_PROPERTY(encoding, getEncoding);
    JSG_READONLY_PROTOTYPE_PROPERTY(fatal, getFatal);
    JSG_READONLY_PROTOTYPE_PROPERTY(ignoreBOM, getIgnoreBOM);

    JSG_TS_OVERRIDE(extends TransformStream<ArrayBuffer | ArrayBufferView, string>);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  jsg::Ref<TextDecoder> decoder;

  void visitForGc(jsg::GcVisitor& visitor);
};

}  // namespace workerd::api
