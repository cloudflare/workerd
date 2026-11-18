// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "byte-chunker.h"

#include <workerd/api/encoding.h>

namespace workerd::api {

ByteChunker::ByteChunker(jsg::Lock& js, jsg::JsString text)
    : length(text.utf8Length(js)),
      remaining(length) {
  if (length > 0) {
    value = Value(text.addRef(js));
  }
}

ByteChunker::ByteChunker(jsg::Lock& js, jsg::JsBufferSource buffer)
    : length(buffer.size()),
      remaining(length) {
  if (length > 0) {
    value = Value(buffer.addRef(js));
  }
}

ByteChunker::ByteChunker(jsg::Ref<Blob> blob): length(blob->getSize()), remaining(length) {
  if (length > 0) {
    value = Value(kj::mv(blob));
  }
}

kj::Maybe<jsg::JsUint8Array> ByteChunker::next(jsg::Lock& js) {
  auto& current = KJ_UNWRAP_OR(value, return kj::none);
  size_t size = kj::min(remaining, kMaxChunkSize);
  kj::Maybe<jsg::JsUint8Array> chunk;
  KJ_SWITCH_ONEOF(current) {
    KJ_CASE_ONEOF(text, jsg::JsRef<jsg::JsString>) {
      chunk = nextTextChunk(js, text.getHandle(js), size);
    }
    KJ_CASE_ONEOF(buffer, jsg::JsRef<jsg::JsBufferSource>) {
      const auto handle = buffer.getHandle(js);
      chunk = jsg::JsUint8Array::create(js, handle.asArrayPtr().slice(offset).first(size));
      offset += size;
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      chunk = jsg::JsUint8Array::create(js, blob->getData(js).slice(offset).first(size));
      offset += size;
    }
  }
  auto& result = KJ_ASSERT_NONNULL(chunk);
  remaining -= result.size();
  if (remaining == 0) {
    value = kj::none;
  }
  return kj::mv(result);
}

jsg::JsUint8Array ByteChunker::nextTextChunk(jsg::Lock& js, jsg::JsString text, size_t size) {
  // Allocate the chunk first: encoding must not cause a V8 GC.
  auto buffer = jsg::JsArrayBuffer::create(js, size);
  auto encoded = encodeUtf8Prefix(js, text, offset, buffer.asArrayPtr().asChars());
  // Any code point fits in four bytes, and a chunk shorter than that holds the rest of the string,
  // so every chunk has something in it.
  KJ_ASSERT(encoded.written > 0, "a string produced fewer bytes than its UTF-8 length");
  offset += encoded.read;
  if (encoded.written == size) {
    return jsg::JsUint8Array::create(js, buffer);
  }
  return buffer.newUint8View(0, encoded.written);
}

}  // namespace workerd::api
