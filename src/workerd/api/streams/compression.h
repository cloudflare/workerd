// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/compression.h>
#include <workerd/api/streams/transform.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>

#include <zlib.h>

namespace workerd::api {

class CompressionStream: public TransformStream {
 public:
  using TransformStream::TransformStream;

  static jsg::Ref<CompressionStream> constructor(jsg::Lock& js, kj::String format);

  // Internal factory for the TypeScript streams implementation: builds the synchronous
  // codec handle (mode is "compress" or "decompress"; decompression picks up the
  // strict_compression_checks flag exactly like the legacy constructor). Registered only
  // under typescript_implemented_streams; the per-isolate bootstrap captures it at load and
  // then REPLACES this global with the TypeScript class, so user code never observes the
  // static.
  static CompressionCodecHandle newCodec(jsg::Lock& js, kj::String mode, kj::String format);

  JSG_RESOURCE_TYPE(CompressionStream, CompatibilityFlags::Reader flags) {
    JSG_INHERIT(TransformStream);

    if (flags.getTypeScriptImplementedStreams()) {
      JSG_STATIC_METHOD(newCodec);
    }

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
