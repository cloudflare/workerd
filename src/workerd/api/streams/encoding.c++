// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "encoding.h"

#include <workerd/api/encoding.h>
#include <workerd/api/streams/standard.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

jsg::Ref<TextEncoderStream> TextEncoderStream::constructor(jsg::Lock& js) {
  auto transformer = TransformStream::constructor(js,
      Transformer{.transform = jsg::Function<Transformer::TransformAlgorithm>(
                      [](jsg::Lock& js, auto chunk, auto controller) {
    auto str = jsg::check(chunk->ToString(js.v8Context()));
    auto utf8Length = str->Utf8LengthV2(js.v8Isolate);

    // Don't emit empty chunks
    if (utf8Length == 0) {
      return js.resolvedPromise();
    }

    v8::Local<v8::ArrayBuffer> buffer;
    JSG_REQUIRE(v8::ArrayBuffer::MaybeNew(js.v8Isolate, utf8Length).ToLocal(&buffer), RangeError,
        "Cannot allocate space for TextEncoder.encode");

    auto bytes = jsg::asBytes(buffer).releaseAsChars();
    [[maybe_unused]] auto written = str->WriteUtf8V2(
        js.v8Isolate, bytes.begin(), bytes.size(), v8::String::WriteFlags::kReplaceInvalidUtf8);

    KJ_DASSERT(written == buffer->ByteLength());
    controller->enqueue(js, v8::Uint8Array::New(buffer, 0, buffer->ByteLength()));
    return js.resolvedPromise();
  })},
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

  auto decoder = TextDecoder::constructor(js, kj::mv(label), options.map([](auto& opts) {
    return TextDecoder::ConstructorOptions{
      .fatal = opts.fatal.orDefault(true),
      .ignoreBOM = opts.ignoreBOM.orDefault(false),
    };
  }));

  // The controller will store c++ references to both the readable and writable
  // streams underlying controllers.
  auto transformer = TransformStream::constructor(js,
      Transformer{.transform = jsg::Function<Transformer::TransformAlgorithm>(
                      JSG_VISITABLE_LAMBDA((decoder = decoder.addRef()), (decoder),
                          (jsg::Lock& js, auto chunk, auto controller) {
                            jsg::BufferSource source(js, chunk);
                            auto decoded = JSG_REQUIRE_NONNULL(
                                decoder->decodePtr(js, source.asArrayPtr(), false), TypeError,
                                "Failed to decode input.");
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
