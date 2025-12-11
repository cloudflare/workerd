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
constexpr char16_t REPLACEMENT_CHAR = 0xFFFD;
constexpr kj::byte REPLACEMENT_UTF8[] = {0xEF, 0xBF, 0xBD};

struct Holder: public kj::Refcounted {
  kj::Maybe<char16_t> pending = kj::none;
};
}  // namespace

// TextEncoderStream encodes a stream of JavaScript strings into UTF-8 bytes.
//
// WHATWG Encoding spec requirement (https://encoding.spec.whatwg.org/#interface-textencoderstream):
// The encoder must handle surrogate pairs that may be split across chunk boundaries.
// This is tested by WPT's "encoding/streams/encode-utf8.any.js" which includes:
//   - "a character split between chunks should be correctly encoded" test
//   - Input: ["\uD83D", "\uDC99"] (U+1F499 💙 split into high/low surrogate chunks)
//   - Expected output: [0xf0, 0x9f, 0x92, 0x99] (U+1F499 encoded as UTF-8)
//
// The main complexity is handling UTF-16 surrogate pairs that may be split across chunks:
// - JavaScript strings use UTF-16 encoding internally
// - A surrogate pair consists of a high surrogate (0xD800-0xDBFF) followed by a low surrogate
//   (0xDC00-0xDFFF), representing code points above U+FFFF (e.g., emoji, rare CJK characters)
// - If a chunk ends with a high surrogate, we must wait for the next chunk to see if it starts
//   with a matching low surrogate before encoding
// - If no match arrives (chunk starts with non-low-surrogate, or stream ends), the orphaned
//   high surrogate is replaced with U+FFFD (replacement character)
//
// State machine:
//   holder->pending = kj::none    -> No pending high surrogate from previous chunk
//   holder->pending = char16_t    -> High surrogate waiting for a matching low surrogate
//
// Transform algorithm for each chunk:
//   1. Allocate buffer with prefix slot if we have a pending surrogate
//   2. Write the chunk's UTF-16 code units into the buffer (after the prefix slot)
//   3. If pending exists:
//      - If chunk starts with low surrogate -> complete the pair (buf[0] = pending lead)
//      - Otherwise -> replace pending with U+FFFD (buf[0] = REPLACEMENT_CHAR)
//   4. If chunk ends with high surrogate -> save it as pending, exclude from output
//   5. Sanitize remaining surrogates with simdutf::to_well_formed_utf16
//   6. Convert to UTF-8 and enqueue
//
// Flush algorithm (when stream closes):
//   - If pending high surrogate exists -> emit U+FFFD (3 UTF-8 bytes: 0xEF 0xBF 0xBD)
//
// Ref: https://github.com/web-platform-tests/wpt/blob/master/encoding/streams/encode-utf8.any.js
jsg::Ref<TextEncoderStream> TextEncoderStream::constructor(jsg::Lock& js) {
  auto state = kj::rc<Holder>();

  auto transform = [holder = state.addRef()](jsg::Lock& js, v8::Local<v8::Value> chunk,
                       jsg::Ref<TransformStreamDefaultController> controller) mutable {
    auto str = jsg::check(chunk->ToString(js.v8Context()));
    size_t length = str->Length();

    // Early exit: empty chunk with no pending surrogate produces no output
    if (length == 0 && holder->pending == kj::none) return js.resolvedPromise();

    // Allocate buffer: reserve slot 0 for pending surrogate if we have one
    size_t prefix = (holder->pending != kj::none) ? 1 : 0;
    auto buf = kj::heapArray<char16_t>(prefix + length);
    str->WriteV2(js.v8Isolate, 0, length, reinterpret_cast<uint16_t*>(buf.begin() + prefix));

    // Handle pending high surrogate from previous chunk
    KJ_IF_SOME(lead, holder->pending) {
      KJ_DASSERT(U_IS_LEAD(lead), "pending must be a high surrogate");
      // Empty chunk: keep pending surrogate for next chunk
      if (length == 0) return js.resolvedPromise();
      holder->pending = kj::none;
      // If chunk starts with matching low surrogate, complete the pair; otherwise emit U+FFFD
      buf[0] = U_IS_TRAIL(buf[prefix]) ? lead : REPLACEMENT_CHAR;
    }

    size_t end = prefix + length;
    KJ_DASSERT(end <= buf.size());

    // If chunk ends with high surrogate, save it for next chunk
    if (end > 0 && U_IS_LEAD(buf[end - 1])) {
      holder->pending = buf[--end];
    }

    // Nothing to encode after handling surrogates
    if (end == 0) return js.resolvedPromise();

    auto slice = buf.first(end);
    KJ_DASSERT(slice.size() > 0);
    auto result = simdutf::utf8_length_from_utf16_with_replacement(slice.begin(), slice.size());
    // Only sanitize if there are unpaired surrogates in the middle of the buffer
    if (result.error == simdutf::error_code::SURROGATE) {
      simdutf::to_well_formed_utf16(slice.begin(), slice.size(), slice.begin());
    }
    auto utf8Length = result.count;
    KJ_DASSERT(utf8Length > 0);

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
