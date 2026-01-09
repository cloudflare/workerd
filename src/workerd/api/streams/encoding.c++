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

  auto transform = [holder = state.addRef()](jsg::Lock& js, v8::Local<v8::Value> chunk,
                       jsg::Ref<TransformStreamDefaultController> controller) mutable {
    auto str = jsg::check(chunk->ToString(js.v8Context()));
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

    auto backingStore = js.allocBackingStore(utf8Length, jsg::Lock::AllocOption::UNINITIALIZED);
    auto dest = kj::ArrayPtr<char>(static_cast<char*>(backingStore->Data()), utf8Length);
    [[maybe_unused]] auto written =
        simdutf::convert_utf16_to_utf8(slice.begin(), slice.size(), dest.begin());
    KJ_DASSERT(written == utf8Length, "simdutf should write exactly utf8Length bytes");

    auto array = v8::Uint8Array::New(
        v8::ArrayBuffer::New(js.v8Isolate, kj::mv(backingStore)), 0, utf8Length);
    controller->enqueue(js, jsg::JsUint8Array(array));
    return js.resolvedPromise();
  };

  auto flush = [holder = state.addRef()](
                   jsg::Lock& js, jsg::Ref<TransformStreamDefaultController> controller) mutable {
    // If stream ends with orphaned high surrogate, emit replacement character
    if (holder->pending != kj::none) {
      auto backingStore = js.allocBackingStore(3, jsg::Lock::AllocOption::UNINITIALIZED);
      memcpy(backingStore->Data(), REPLACEMENT_UTF8, 3);
      auto array =
          v8::Uint8Array::New(v8::ArrayBuffer::New(js.v8Isolate, kj::mv(backingStore)), 0, 3);
      controller->enqueue(js, jsg::JsUint8Array(array));
    }
    return js.resolvedPromise();
  };

  auto transformer = TransformStream::constructor(js,
      Transformer{.transform = jsg::Function<Transformer::TransformAlgorithm>(kj::mv(transform)),
        .flush = jsg::Function<Transformer::FlushAlgorithm>(kj::mv(flush))},
      StreamQueuingStrategy{}, StreamQueuingStrategy{});

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

  // The controller will store c++ references to both the readable and writable
  // streams underlying controllers.
  auto transformer = TransformStream::constructor(js,
      Transformer{.transform = jsg::Function<Transformer::TransformAlgorithm>( JSG_VISITABLE_LAMBDA(
                      (decoder = decoder.addRef()), (decoder),
                      (jsg::Lock& js, auto chunk, auto controller) {
                        JSG_REQUIRE(chunk->IsArrayBuffer() || chunk->IsArrayBufferView(), TypeError,
                            "This TransformStream is being used as a byte stream, "
                            "but received a value that is not a BufferSource.");
                        jsg::BufferSource source(js, chunk);
                        auto decoded =
                            JSG_REQUIRE_NONNULL(decoder->decodePtr(js, source.asArrayPtr(), false),
                                TypeError, "Failed to decode input.");
                        // Only enqueue if there's actual output - don't emit empty chunks
                        // for incomplete multi-byte sequences
                        if (decoded.length(js) > 0) {
                        controller->enqueue(js, decoded);
                        }
                        return js.resolvedPromise();
                      })),
        .flush = jsg::Function<Transformer::FlushAlgorithm>(
            JSG_VISITABLE_LAMBDA((decoder = decoder.addRef()), (decoder),
                (jsg::Lock& js, auto controller) {
                  auto decoded =
                      JSG_REQUIRE_NONNULL(decoder->decodePtr(js, kj::ArrayPtr<kj::byte>(), true),
                          TypeError, "Failed to decode input.");
                  // Only enqueue if there's actual output
                  if (decoded.length(js) > 0) {
                  controller->enqueue(js, decoded);
                  }
                  return js.resolvedPromise();
                }))},
      StreamQueuingStrategy{}, StreamQueuingStrategy{});

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
