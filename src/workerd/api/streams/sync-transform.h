// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/js-readable-stream.h>
#include <workerd/api/js-writable-stream.h>

#include <rust/cxx.h>

namespace workerd::api {

// A byte transform that runs synchronously: each input chunk is consumed in full as it arrives,
// and whatever output it completes is returned at once. newSyncTransformStreams() supplies all of
// the asynchrony, so a stage only has to implement the transform itself.
//
// A stage is driven from the KJ event loop, possibly without the isolate lock held, so it must
// not touch JavaScript or anything the isolate owns.
class SyncTransformStage {
 public:
  virtual ~SyncTransformStage() noexcept(false) = default;

  // Consumes all of `input`, setting `output`, which starts empty, to any output it completes.
  // Throws if the input is invalid; the stream then errors once the output set before the throw
  // has been read.
  virtual void push(kj::ArrayPtr<const kj::byte> input, kj::Array<kj::byte>& output) = 0;

  // Called once, when the input has ended. Sets `output` to any remaining output, and throws if
  // the input was incomplete.
  virtual void end(kj::Array<kj::byte>& output) = 0;

  // Called once if the stream is canceled or aborted before the stage has failed or ended. The
  // stage is not used again.
  virtual void cancel(const kj::Exception& reason) {}

  // The total number of bytes the stage will produce, if it is known before the input arrives.
  virtual kj::Maybe<uint64_t> getOutputLength() const {
    return kj::none;
  }

  // The bytes the stage holds between calls, such as a partial block or its configuration, so
  // that they can be charged to the isolate as external memory from the moment the stream is
  // created.
  virtual size_t retainedBytes() const {
    return 0;
  }
};

// Takes the bytes a Rust stage produced as stage output. The stream charges and bounds its queue
// by the bytes in it, so the allocation is kept only if it is close to their size; otherwise
// they are copied out and the allocation freed.
kj::Array<kj::byte> fromRustBytes(::rust::Vec<uint8_t> bytes);

// Returns the two sides of a byte-only transform stream that runs `stage` over everything written
// to `writable` and makes the output readable from `readable`. Both sides are native, so a pipe
// between them and another native stream runs entirely in C++.
//
// Input is passed to the stage at most `highWaterMark` bytes at a time, and each increment waits
// until the output still waiting to be read is at most `highWaterMark` bytes, as a
// TransformStream's second write waits for its readable side to be read. So at most
// `highWaterMark` bytes, plus the output of one increment, are queued at any time, however large
// the writes are. The queued output and the stage's retained bytes are charged to the isolate as
// external memory, from the moment the streams are created.
//
// If the stage knows its output length, the readable side reports it, and holds back the last
// byte until the stage has ended successfully. So a consumer that stops reading once it has the
// whole length still sees an error that the end of the input reveals.
JsReadableWritablePair newSyncTransformStreams(
    jsg::Lock& js, IoContext& ioContext, kj::Own<SyncTransformStage> stage, size_t highWaterMark);

}  // namespace workerd::api
