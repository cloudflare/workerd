// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/blob.h>
#include <workerd/api/encoding.h>
#include <workerd/jsg/jsg.h>

#include <kj/one-of.h>

namespace workerd::api {

// Produces the bytes of a JavaScript string (as UTF-8), BufferSource or Blob as a sequence of new
// Uint8Arrays, for a byte stream to enqueue. A value of at most kMaxChunkSize bytes is produced
// as one chunk, and a longer one in chunks of up to that size. Each chunk of a string ends on a
// code point boundary, and a lone surrogate is written as the `loneSurrogates` the chunker was
// made with says; with LoneSurrogates::REPLACE, every chunk is valid UTF-8.
//
// Every chunk is a copy, so the value itself is never handed to JavaScript, transferred or
// detached. The chunker drops the value once it has produced every byte, or when cancelled.
class ByteChunker final {
 public:
  static constexpr size_t kMaxChunkSize = 128 * 1024;

  ByteChunker(
      jsg::Lock& js, jsg::JsString text, LoneSurrogates loneSurrogates = LoneSurrogates::REPLACE);
  ByteChunker(jsg::Lock& js, jsg::JsBufferSource buffer);
  explicit ByteChunker(jsg::Ref<Blob> blob);

  KJ_DISALLOW_COPY_AND_MOVE(ByteChunker);

  // The number of bytes the value produces in all.
  uint64_t getLength() const {
    return length;
  }

  // The size of the largest chunk next() returns.
  size_t getMaxChunkSize() const {
    return kj::min(length, kMaxChunkSize);
  }

  // True once every byte has been produced, or the chunker was cancelled.
  bool isDone() const {
    return value == kj::none;
  }

  // Returns the next chunk, or kj::none when there are no more.
  kj::Maybe<jsg::JsUint8Array> next(jsg::Lock& js);

  // Drops the value, so that next() produces no more chunks.
  void cancel() {
    value = kj::none;
  }

 private:
  using Value =
      kj::OneOf<jsg::JsRef<jsg::JsString>, jsg::JsRef<jsg::JsBufferSource>, jsg::Ref<Blob>>;

  // None once done. The value is a string, an ArrayBuffer, a view or a Blob, none of which can
  // refer to a stream, so holding it strongly cannot keep a stream that holds this chunker alive.
  kj::Maybe<Value> value;
  uint64_t length;

  // The bytes still to produce.
  uint64_t remaining;

  // Where the next chunk starts: in UTF-16 code units for a string, in bytes otherwise.
  size_t offset = 0;

  LoneSurrogates loneSurrogates = LoneSurrogates::REPLACE;

  jsg::JsUint8Array nextTextChunk(jsg::Lock& js, jsg::JsString text, size_t size);
};

}  // namespace workerd::api
