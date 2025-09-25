#pragma once

#include "transform.h"

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// The IdentityTransformStream is a non-standard TransformStream implementation that passes
// the exact bytes written to the writable side on to the readable side without modification.
// Unlike standard the TransformStream, the readable side of an IdentityTransformStream
// supports BYOB reads.
//
// The IdentityTransformStream is a kj-based implementation backed by a ReadableStreamSource
// and WritableStreamSink implementation. It is a legacy class that was created before the
// standard TransformStream constructor was available in workers. It is maintained for
// backwards compatibility but otherwise has no special significance.
class IdentityTransformStream: public TransformStream {
 public:
  using TransformStream::TransformStream;

  struct QueuingStrategy {
    jsg::Optional<uint64_t> highWaterMark;

    JSG_STRUCT(highWaterMark);
  };

  static jsg::Ref<IdentityTransformStream> constructor(
      jsg::Lock& js, jsg::Optional<QueuingStrategy> queuingStrategy = kj::none);

  JSG_RESOURCE_TYPE(IdentityTransformStream) {
    JSG_INHERIT(TransformStream);

    JSG_TS_OVERRIDE(extends TransformStream<ArrayBuffer | ArrayBufferView, Uint8Array>);
  }
};

// Same as an IdentityTransformStream, except with a known length in bytes on the readable side.
// We don't currently enforce this limit -- it just convinces the kj-http layer to
// emit a Content-Length (assuming it doesn't get gzipped or anything).
class FixedLengthStream: public IdentityTransformStream {
 public:
  using IdentityTransformStream::IdentityTransformStream;

  static jsg::Ref<FixedLengthStream> constructor(jsg::Lock& js,
      uint64_t expectedLength,
      jsg::Optional<QueuingStrategy> queuingStrategy = kj::none);

  JSG_RESOURCE_TYPE(FixedLengthStream) {
    JSG_INHERIT(IdentityTransformStream);
  }
};

struct OneWayPipe {
  kj::Own<ReadableStreamSource> in;
  kj::Own<WritableStreamSink> out;
};

OneWayPipe newIdentityPipe(kj::Maybe<uint64_t> expectedLength = kj::none);

bool isIdentityTransformStream(WritableStreamSink& sink);

}  // namespace workerd::api
