// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "transform.h"
#include <workerd/jsg/jsg.h>

namespace workerd::api {

class CompressionStream: public TransformStream {
public:
  using TransformStream::TransformStream;

  static jsg::Ref<CompressionStream> constructor(jsg::Lock& js, kj::String format);

  JSG_RESOURCE_TYPE(CompressionStream) {
    JSG_INHERIT(TransformStream);

    JSG_TS_OVERRIDE(extends TransformStream<ArrayBuffer | ArrayBufferView, Uint8Array> { constructor(format
                                 : "gzip" | "deflate" | "deflate-raw");
    });
  }
};

class DecompressionStream: public TransformStream {
public:
  using TransformStream::TransformStream;

  static jsg::Ref<DecompressionStream> constructor(jsg::Lock& js, kj::String format);

  JSG_RESOURCE_TYPE(DecompressionStream) {
    JSG_INHERIT(TransformStream);

    JSG_TS_OVERRIDE(extends TransformStream<ArrayBuffer | ArrayBufferView, Uint8Array> { constructor(format
                                 : "gzip" | "deflate" | "deflate-raw");
    });
  }
};

}  // namespace workerd::api
