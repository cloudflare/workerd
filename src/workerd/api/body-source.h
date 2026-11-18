// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/blob.h>
#include <workerd/api/js-readable-stream.h>
#include <workerd/jsg/jsg.h>

#include <kj/one-of.h>
#include <kj/refcount.h>

namespace workerd::api {

// The in-memory source a Body was extracted from: a byte sequence, a string, a BufferSource or a
// Blob. This is what the Fetch spec calls the body's source. A Body keeps its source so that it
// creates the body's stream only when the stream is first needed, and creates a fresh one each
// time the body has to be sent again, as after a redirect or for a retried Durable Object call.
//
// A stream created from a source reads it from the start and never modifies it, so a source can
// be shared: clone() returns another BodySource over the same value, without copying it.
class BodySource final {
 public:
  explicit BodySource(kj::Array<const kj::byte> bytes);
  explicit BodySource(kj::String text);
  BodySource(jsg::Lock& js, jsg::JsString text);
  // `buffer` must not be reachable from JavaScript: pass the copy that extracting a body makes of
  // a BufferSource.
  BodySource(jsg::Lock& js, jsg::JsBufferSource buffer);
  explicit BodySource(jsg::Ref<Blob> blob);

  BodySource(BodySource&&) = default;
  BodySource& operator=(BodySource&&) = default;
  KJ_DISALLOW_COPY(BodySource);

  // Returns another BodySource over the same value.
  BodySource clone(jsg::Lock& js);

  // Returns a new stream that reads the source from the start. Requires the current IoContext.
  JsReadableStream createStream(jsg::Lock& js);

  // The number of bytes a stream created from the source produces, for StreamEncoding::IDENTITY.
  // kj::none for any other encoding, as for any in-memory stream.
  kj::Maybe<uint64_t> tryGetLength(jsg::Lock& js, StreamEncoding encoding);

  void visitForGc(jsg::GcVisitor& visitor);
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  // The bytes are shared by every stream created from the source, which reads them in place.
  using Bytes = kj::Rc<kj::Array<const kj::byte>>;
  using Value =
      kj::OneOf<Bytes, jsg::JsRef<jsg::JsString>, jsg::JsRef<jsg::JsBufferSource>, jsg::Ref<Blob>>;

  BodySource(Value value, kj::Maybe<uint64_t> stringLength)
      : value(kj::mv(value)),
        stringLength(stringLength) {}

  Value value;

  // The UTF-8 length of a string source, computed when first needed.
  kj::Maybe<uint64_t> stringLength;
};

}  // namespace workerd::api
