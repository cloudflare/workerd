// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "transform.h"

#include <workerd/jsg/jsg.h>

#include <zlib.h>

namespace workerd::api {

// A custom allocator to be used by the zlib and brotli libraries.
// The allocator should not and can not safely hold a reference to the jsg::Lock
// instance. Therefore, we lookup the current jsg::Lock instance from the
// isolate pointer and use that to get the external memory adjustment.
class CompressionAllocator final {
public:
  void configure(z_stream* stream);

  static void* AllocForZlib(void* data, uInt items, uInt size);
  static void* AllocForBrotli(void* data, size_t size);
  static void FreeForZlib(void* data, void* pointer);

private:
  struct Allocation {
    kj::Array<kj::byte> data;
    jsg::ExternalMemoryAdjustment memoryAdjustment;
  };
  kj::HashMap<void*, Allocation> allocations;
};

class CompressionStream: public TransformStream {
public:
  using TransformStream::TransformStream;

  static jsg::Ref<CompressionStream> constructor(kj::String format);

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
