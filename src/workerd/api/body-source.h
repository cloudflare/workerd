// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/blob.h>
#include <workerd/api/js-readable-stream.h>
#include <workerd/api/streams/common.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/strong-bool.h>

#include <kj/one-of.h>

namespace workerd::api {

// Whether BodySource::pumpTo() offers each chunk to its sink through tryWriteSync(), under the
// isolate lock, before writing it asynchronously. A sink that does work for each write, such as
// compressing it, would do that work under the lock, so pass NO for one.
WD_STRONG_BOOL(TryWriteSync);

// The in-memory source a Body was extracted from: a string, a BufferSource or a Blob, each a
// JavaScript value that the isolate accounts for. This is what the Fetch spec calls the body's
// source. A Body keeps its source so that it creates the body's stream only when the stream is
// first needed, and creates a fresh one each time the body has to be sent again, as after a
// redirect or for a retried Durable Object call.
//
// A stream created from a source reads it from the start and never modifies it, so a source can
// be shared: clone() returns another BodySource over the same value, without copying it.
class BodySource final {
 public:
  BodySource(jsg::Lock& js, jsg::JsString text);
  // `buffer` must not be reachable from JavaScript: pass the copy that extracting a body makes of
  // a BufferSource.
  BodySource(jsg::Lock& js, jsg::JsBufferSource buffer);
  // Copies bytes that C++ produced, such as a serialized FormData, into a new Uint8Array.
  BodySource(jsg::Lock& js, kj::ArrayPtr<const kj::byte> bytes);
  explicit BodySource(jsg::Ref<Blob> blob);

  BodySource(BodySource&&) = default;
  BodySource& operator=(BodySource&&) = default;
  KJ_DISALLOW_COPY(BodySource);

  // Returns another BodySource over the same value.
  BodySource clone(jsg::Lock& js) KJ_WARN_UNUSED_RESULT;

  // Returns a new stream that reads the source from the start.
  JsReadableStream createStream(jsg::Lock& js) KJ_WARN_UNUSED_RESULT;

  // A new ArrayBuffer holding the bytes createStream() produces.
  jsg::JsArrayBuffer readArrayBuffer(jsg::Lock& js) KJ_WARN_UNUSED_RESULT;

  // The bytes createStream() produces, as text, as JsReadableStream::text() reads them: that
  // removes a leading UTF-8 byte order mark under some flags.
  kj::String readText(jsg::Lock& js) KJ_WARN_UNUSED_RESULT;

  // The number of bytes a stream created from the source produces, for StreamEncoding::IDENTITY.
  // kj::none for any other encoding, as for any in-memory stream.
  kj::Maybe<uint64_t> tryGetLength(jsg::Lock& js, StreamEncoding encoding) KJ_WARN_UNUSED_RESULT;

  // Writes the bytes createStream() produces to `sink`, then ends it, without a stream and without
  // running JavaScript. Must be called within an IoContext, under its isolate lock.
  //
  // The source is read in chunks of up to ByteChunker::kMaxChunkSize, under the isolate lock, since
  // its bytes are only accessible while the lock is held. Unless `tryWriteSync` is NO, each chunk is
  // offered to the sink with tryWriteSync(): a BufferSource's or Blob's straight from its memory,
  // a string's once encoded into a native scratch buffer. A chunk the sink does not take is copied
  // into the scratch buffer, if it is not there already, and written asynchronously, with the lock
  // released; the pump takes the lock again for the next chunk. So that a sink that takes every
  // chunk synchronously does not hold the lock for the whole body, the pump releases the lock and
  // takes it again after every few synchronous writes.
  //
  // The first chunks are read before pumpTo() returns. The pump takes the lock again in the
  // critical section that was current when pumpTo() was called, as awaitIo() continues in it,
  // so that a body sent from within blockConcurrencyWhile() can be read while the callback awaits
  // the send.
  //
  // The whole pump needs the IoContext, not only its reads: the sink's writes may depend on it,
  // as a system stream's do, each registering a pending event. So the caller must not let the pump
  // outlive the IoContext, as deferred proxying would.
  //
  // Each write is part of the message the body belongs to, which the caller sends once the
  // actor's output gate allows it (as respondWith() and fetch() do), so the pump does not wait
  // for the output gate again: the bytes were all decided when the message was sent.
  kj::Promise<void> pumpTo(jsg::Lock& js,
      kj::Own<WritableStreamSink> sink,
      TryWriteSync tryWriteSync) KJ_WARN_UNUSED_RESULT;

  void visitForGc(jsg::GcVisitor& visitor);
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  class Pump;

  using Value =
      kj::OneOf<jsg::JsRef<jsg::JsString>, jsg::JsRef<jsg::JsBufferSource>, jsg::Ref<Blob>>;

  BodySource(Value value, kj::Maybe<uint64_t> stringLength)
      : value(kj::mv(value)),
        stringLength(stringLength) {}

  // The source's bytes, valid while the isolate lock is held, or kj::none for a string, whose
  // bytes must be encoded.
  kj::Maybe<kj::ArrayPtr<const kj::byte>> tryGetBytes(jsg::Lock& js) KJ_LIFETIMEBOUND;

  // Encodes a string source into `output`, which must be as long as its UTF-8 length.
  void encodeText(jsg::Lock& js, jsg::JsString text, kj::ArrayPtr<char> output);

  Value value;

  // The UTF-8 length of a string source, computed when first needed.
  kj::Maybe<uint64_t> stringLength;
};

}  // namespace workerd::api
