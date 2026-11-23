// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/js-readable-stream.h>
#include <workerd/api/js-writable-stream.h>

#include <kj/vector.h>

namespace workerd::api {

// A byte transform that runs synchronously: each input chunk is consumed in full as it arrives,
// and whatever output it completes is returned at once. newSyncTransformStreams() supplies all of
// the asynchrony, so a stage only has to implement the transform itself.
//
// A stage is driven from the KJ event loop, possibly without the isolate lock held, so it must
// not touch JavaScript or anything the isolate owns.
class SyncTransformStage {
 public:
  using Output = kj::Vector<kj::Array<kj::byte>>;

  virtual ~SyncTransformStage() noexcept(false) = default;

  // Consumes all of `input`, appending any output it completes to `output`. Throws if the input
  // is invalid; the stream then errors once the output already produced has been read.
  virtual void push(kj::ArrayPtr<const kj::byte> input, Output& output) = 0;

  // Called once, when the input has ended. Appends any remaining output, and throws if the input
  // was incomplete.
  virtual void end(Output& output) = 0;

  // Called once if the stream is canceled or aborted before the stage has failed or ended. The
  // stage is not used again.
  virtual void cancel(const kj::Exception& reason) {}

  // The total number of bytes the stage will produce, if it is known before the input arrives.
  virtual kj::Maybe<uint64_t> getOutputLength() const {
    return kj::none;
  }

  // The bytes the stage holds between calls, such as a partial block, so that they can be
  // charged to the isolate as external memory.
  virtual size_t retainedBytes() const {
    return 0;
  }
};

struct SyncTransformStreams {
  JsReadableStream readable;
  JsWritableStream writable;
};

// Returns the two sides of a byte-only transform stream that runs `stage` over everything written
// to `writable` and makes the output readable from `readable`. Both sides are native, so a pipe
// between them and another native stream runs entirely in C++.
//
// Input is passed to the stage at most `highWaterMark` bytes at a time, and each increment waits
// until the output still waiting to be read is at most `highWaterMark` bytes, as a
// TransformStream's second write waits for its readable side to be read. So at most
// `highWaterMark` bytes, plus the output of one increment, are queued at any time, however large
// the writes are. The queued output and the stage's retained bytes are charged to the isolate as
// external memory.
//
// If the stage knows its output length, the readable side reports it, and holds back the last
// byte until the stage has ended successfully. So a consumer that stops reading once it has the
// whole length still sees an error that the end of the input reveals.
SyncTransformStreams newSyncTransformStreams(
    jsg::Lock& js, IoContext& ioContext, kj::Own<SyncTransformStage> stage, size_t highWaterMark);

}  // namespace workerd::api
