// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "encoding.h"

#include "simdutf.h"

#include <workerd/api/encoding.h>
#include <workerd/api/streams/standard.h>
#include <workerd/io/features.h>
#include <workerd/jsg/jsg.h>

#include <v8.h>

#include <kj/common.h>
#include <kj/refcount.h>

namespace workerd::api {

namespace {
constexpr kj::byte REPLACEMENT_UTF8[] = {0xEF, 0xBF, 0xBD};

struct Holder: public kj::Refcounted {
  kj::Maybe<char16_t> pending = kj::none;
};
}  // namespace

// TextEncoderStream encodes a stream of JavaScript strings into UTF-8 bytes.
//
// WHATWG Encoding spec requirement (https://encoding.spec.whatwg.org/#interface-textencoderstream):
// The encoder must encode unpaired UTF-16 surrogates as replacement characters.
//
// simdutf handles this for us, but we have to be careful of surrogate pairs
//   (high surrogate, followed by low surrogate) split across chunk boundaries.
//
// We do this with the pending field:
//   holder->pending = kj::none    -> No pending high surrogate from previous chunk
//   holder->pending = char16_t    -> High surrogate waiting for a matching low surrogate
//
// Ref: https://github.com/web-platform-tests/wpt/blob/master/encoding/streams/encode-utf8.any.js
jsg::Ref<TextEncoderStream> TextEncoderStream::constructor(jsg::Lock& js) {
  auto state = kj::rc<Holder>();

  auto transform = [holder = state.addRef()](jsg::Lock& js, jsg::JsValue chunk,
                       jsg::Ref<TransformStreamDefaultController> controller) mutable {
    v8::Local<v8::String> str = chunk.toJsString(js);
    size_t length = str->Length();
    if (length == 0) return js.resolvedPromise();

    // Allocate buffer: reserve slot 0 for pending surrogate if we have one
    size_t prefix = (holder->pending == kj::none) ? 0 : 1;
    size_t end = prefix + length;
    auto buf = kj::heapArray<char16_t>(end);
    str->WriteV2(js.v8Isolate, 0, length, reinterpret_cast<uint16_t*>(buf.begin() + prefix));

    KJ_IF_SOME(lead, holder->pending) {
      buf.begin()[0] = lead;
      holder->pending = kj::none;
    }

    // If chunk ends with high surrogate, save it for next chunk
    if (end > 0 && U_IS_LEAD(buf[end - 1])) {
      holder->pending = buf[--end];
    }
    if (end == 0) return js.resolvedPromise();

    auto slice = buf.first(end);
    auto result = simdutf::utf8_length_from_utf16_with_replacement(slice.begin(), slice.size());
    // Only sanitize if there are surrogates in the buffer - UTF-16 without
    // surrogates is always well-formed.
    if (result.error == simdutf::error_code::SURROGATE) {
      simdutf::to_well_formed_utf16(slice.begin(), slice.size(), slice.begin());
    }
    auto utf8Length = result.count;
    KJ_DASSERT(utf8Length > 0 && utf8Length >= end);

    auto dest = jsg::JsArrayBuffer::create(js, utf8Length);
    [[maybe_unused]] auto written = simdutf::convert_utf16_to_utf8(
        slice.begin(), slice.size(), dest.asArrayPtr().asChars().begin());
    KJ_DASSERT(written == utf8Length, "simdutf should write exactly utf8Length bytes");

    auto u8 = jsg::JsUint8Array::create(js, dest);
    controller->enqueue(js, u8);
    return js.resolvedPromise();
  };

  auto flush = [holder = state.addRef()](
                   jsg::Lock& js, jsg::Ref<TransformStreamDefaultController> controller) mutable {
    // If stream ends with orphaned high surrogate, emit replacement character
    if (holder->pending != kj::none) {
      auto u8 = jsg::JsUint8Array::create(js, 3);
      u8.asArrayPtr().copyFrom(REPLACEMENT_UTF8);
      controller->enqueue(js, u8);
    }
    return js.resolvedPromise();
  };

  // Batch transform: encode multiple queued string chunks into UTF-8 output.
  // Processes chunks in sub-batches to bound memory usage — without this,
  // a large queue (e.g., 500K tiny writes) would allocate massive transient
  // buffers causing GC pressure and progressive slowdown.
  auto transformv = [holder = state.addRef()](jsg::Lock& js,
                        kj::Array<jsg::JsRef<jsg::JsValue>> chunks,
                        jsg::Ref<TransformStreamDefaultController> controller) mutable {
    static constexpr size_t kMaxBatchUtf8Bytes = 64 * 1024;  // 64KB of UTF-8 output

    size_t i = 0;
    while (i < chunks.size()) {
      // Collect a sub-batch up to the byte limit.
      v8::LocalVector<v8::String> strings(js.v8Isolate);
      size_t totalLength = 0;
      while (i < chunks.size()) {
        auto str = chunks[i].getHandle(js).toJsString(js);
        size_t len = str.utf8Length(js);
        if (totalLength + len > kMaxBatchUtf8Bytes && totalLength > 0) {
          // This chunk would exceed the limit and we already have data.
          break;
        }
        strings.push_back(str);
        totalLength += len;
        i++;
      }

      if (totalLength == 0 && holder->pending == kj::none) continue;

      // Allocate a single contiguous buffer for this sub-batch.
      size_t prefix = (holder->pending == kj::none) ? 0 : 1;
      size_t end = prefix + totalLength;
      auto buf = kj::heapArray<char16_t>(end);

      // Copy pending surrogate into slot 0 if present.
      KJ_IF_SOME(lead, holder->pending) {
        buf.begin()[0] = lead;
        holder->pending = kj::none;
      }

      // Copy all string contents into the buffer.
      size_t offset = prefix;
      for (auto& str: strings) {
        size_t len = str->Length();
        if (len > 0) {
          str->WriteV2(js.v8Isolate, 0, len, reinterpret_cast<uint16_t*>(buf.begin() + offset));
          offset += len;
        }
      }
      KJ_DASSERT(offset == end);

      // If buffer ends with high surrogate, save it for next sub-batch.
      if (end > 0 && U_IS_LEAD(buf[end - 1])) {
        holder->pending = buf[--end];
      }
      if (end == 0) continue;

      // Encode to UTF-8 and enqueue.
      auto slice = buf.first(end);
      auto result = simdutf::utf8_length_from_utf16_with_replacement(slice.begin(), slice.size());
      if (result.error == simdutf::error_code::SURROGATE) {
        simdutf::to_well_formed_utf16(slice.begin(), slice.size(), slice.begin());
      }
      auto utf8Length = result.count;
      KJ_DASSERT(utf8Length > 0 && utf8Length >= end);

      auto dest = jsg::JsArrayBuffer::create(js, utf8Length);
      [[maybe_unused]] auto written = simdutf::convert_utf16_to_utf8(
          slice.begin(), slice.size(), dest.asArrayPtr().asChars().begin());
      KJ_DASSERT(written == utf8Length);

      auto u8 = jsg::JsUint8Array::create(js, dest);
      controller->enqueue(js, u8);
    }

    return js.resolvedPromise();
  };

  // Per the WHATWG Encoding spec, the readable side HWM should be 0, so writes
  // block until a reader pulls. Previously StreamQueuingStrategy{} was passed,
  // which bypassed the orDefault() in TransformStream::constructor and caused
  // the readable HWM to default to 1, clearing backpressure at startup.
  // Passing kj::none lets TransformStream apply the spec defaults (writable HWM=1,
  // readable HWM=0).
  kj::Maybe<StreamQueuingStrategy> readableStrategy;
  if (!FeatureFlags::get(js).getEncoderStreamSpecCompliantBackpressure()) {
    readableStrategy = StreamQueuingStrategy{};
  }

  auto transformer = TransformStream::constructor(js,
      Transformer{
        .transform = jsg::Function<Transformer::TransformAlgorithm>(kj::mv(transform)),
        .flush = jsg::Function<Transformer::FlushAlgorithm>(kj::mv(flush)),
        .transformv = jsg::Function<Transformer::TransformvAlgorithm>(kj::mv(transformv)),
      },
      StreamQueuingStrategy{}, kj::mv(readableStrategy));

  return js.alloc<TextEncoderStream>(transformer->getReadable(), transformer->getWritable());
}

TextDecoderStream::TextDecoderStream(jsg::Ref<TextDecoder> decoder,
    jsg::Ref<ReadableStream> readable,
    jsg::Ref<WritableStream> writable)
    : TransformStream(kj::mv(readable), kj::mv(writable)),
      decoder(kj::mv(decoder)) {}

jsg::Ref<TextDecoderStream> TextDecoderStream::constructor(
    jsg::Lock& js, jsg::Optional<kj::String> label, jsg::Optional<TextDecoderStreamInit> options) {

  auto decoder = TextDecoder::constructor(js, kj::mv(label), options.map([&js](auto& opts) {
    return TextDecoder::ConstructorOptions{
      // Previously this would default to true. The spec requires a default
      // of false, however. When the pedanticWpt flag is not set, we continue
      // to default as true.
      .fatal = opts.fatal.orDefault(!FeatureFlags::get(js).getPedanticWpt()),
      .ignoreBOM = opts.ignoreBOM.orDefault(false),
    };
  }));

  auto transform = JSG_VISITABLE_LAMBDA(
      (decoder = decoder.addRef()), (decoder), (jsg::Lock& js, auto chunk, auto controller) {
        JSG_REQUIRE(
            chunk.isArrayBuffer() || chunk.isSharedArrayBuffer() || chunk.isArrayBufferView(),
            TypeError,
            "This TransformStream is being used as a byte stream, "
            "but received a value that is not a BufferSource.");
        jsg::JsBufferSource source(chunk);
        auto decoded = JSG_REQUIRE_NONNULL(decoder->decodePtr(js, source.asArrayPtr(), false),
            TypeError, "Failed to decode input.");
        // Only enqueue if there's actual output - don't emit empty chunks
        // for incomplete multi-byte sequences
        if (decoded.length(js) > 0) {
        controller->enqueue(js, decoded);
        }
        return js.resolvedPromise();
      });

  auto flush = JSG_VISITABLE_LAMBDA(
      (decoder = decoder.addRef()), (decoder), (jsg::Lock& js, auto controller) {
        auto decoded = JSG_REQUIRE_NONNULL(decoder->decodePtr(js, kj::ArrayPtr<kj::byte>(), true),
            TypeError, "Failed to decode input.");
        // Only enqueue if there's actual output
        if (decoded.length(js) > 0) {
        controller->enqueue(js, decoded);
        }
        return js.resolvedPromise();
      });

  // Batch transform: concatenate multiple byte chunks and decode in sub-batches.
  // Bounds memory usage for large queues while reducing per-chunk decodePtr and
  // enqueue calls. The TextDecoder's internal state handles partial multi-byte
  // sequences at sub-batch boundaries.
  auto transformv = JSG_VISITABLE_LAMBDA((decoder = decoder.addRef()), (decoder),
      (jsg::Lock& js, kj::Array<jsg::JsRef<jsg::JsValue>> chunks, auto controller) {
        static constexpr size_t kMaxBatchBytes = 64 * 1024;  // 64KB per sub-batch

        size_t i = 0;
        while (i < chunks.size()) {
        // Collect a sub-batch up to the byte limit.
        size_t totalSize = 0;
        size_t batchStart = i;
        while (i < chunks.size()) {
        auto handle = chunks[i].getHandle(js);
        JSG_REQUIRE(
            handle.isArrayBuffer() || handle.isSharedArrayBuffer() || handle.isArrayBufferView(),
            TypeError,
            "This TransformStream is being used as a byte stream, "
            "but received a value that is not a BufferSource.");
        jsg::JsBufferSource source(handle);
        size_t len = source.size();
        if (totalSize + len > kMaxBatchBytes && totalSize > 0) {
        break;
        }
        totalSize += len;
        i++;
        }

        if (totalSize == 0) continue;

        // Concatenate this sub-batch into a single buffer.
        auto combined = kj::heapArray<kj::byte>(totalSize);
        size_t offset = 0;
        for (size_t j = batchStart; j < i; j++) {
        jsg::JsBufferSource source(chunks[j].getHandle(js));
        auto ptr = source.asArrayPtr();
        combined.slice(offset, offset + ptr.size()).copyFrom(ptr);
        offset += ptr.size();
        }
        KJ_DASSERT(offset == totalSize);

        // Decode and enqueue.
        auto decoded = JSG_REQUIRE_NONNULL(
            decoder->decodePtr(js, combined.asPtr(), false), TypeError, "Failed to decode input.");
        if (decoded.length(js) > 0) {
        controller->enqueue(js, decoded);
        }
        }

        return js.resolvedPromise();
      });

  // The controller will store c++ references to both the readable and writable
  // streams underlying controllers.
  // See comment in TextEncoderStream::constructor for why we conditionally pass
  // kj::none for the readable strategy.
  kj::Maybe<StreamQueuingStrategy> readableStrategy;
  if (!FeatureFlags::get(js).getEncoderStreamSpecCompliantBackpressure()) {
    readableStrategy = StreamQueuingStrategy{};
  }
  auto transformer = TransformStream::constructor(js,
      Transformer{
        .transform = jsg::Function<Transformer::TransformAlgorithm>(kj::mv(transform)),
        .flush = jsg::Function<Transformer::FlushAlgorithm>(kj::mv(flush)),
        .transformv = jsg::Function<Transformer::TransformvAlgorithm>(kj::mv(transformv)),
      },
      StreamQueuingStrategy{}, kj::mv(readableStrategy));

  return js.alloc<TextDecoderStream>(
      kj::mv(decoder), transformer->getReadable(), transformer->getWritable());
}

kj::StringPtr TextDecoderStream::getEncoding() {
  return decoder->getEncoding();
}

bool TextDecoderStream::getFatal() {
  return decoder->getFatal();
}

bool TextDecoderStream::getIgnoreBOM() {
  return decoder->getIgnoreBom();
}

void TextDecoderStream::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("decoder", decoder);
}

void TextDecoderStream::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(decoder);
}

}  // namespace workerd::api
