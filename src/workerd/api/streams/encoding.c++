// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "encoding.h"

#include "../encoding.h"
#include "standard.h"

#include <workerd/jsg/jsg.h>

namespace workerd::api {

jsg::Ref<TextEncoderStream> TextEncoderStream::constructor(jsg::Lock& js) {
  auto transformer = TransformStream::constructor(js,
      Transformer{.transform = jsg::Function<Transformer::TransformAlgorithm>(
                      [](jsg::Lock& js, auto chunk, auto controller) {
    auto str = jsg::check(chunk->ToString(js.v8Context()));
    auto maybeBuffer = v8::ArrayBuffer::MaybeNew(js.v8Isolate, str->Utf8LengthV2(js.v8Isolate));
    JSG_ASSERT(!maybeBuffer.IsEmpty(), RangeError, "Cannot allocate space for TextEncoder.encode");
    auto buffer = maybeBuffer.ToLocalChecked();

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
      Transformer{.transform = jsg::Function<Transformer::TransformAlgorithm>( JSG_VISITABLE_LAMBDA(
                      (decoder = decoder.addRef()), (decoder),
                      (jsg::Lock& js, auto chunk, auto controller) {
                        jsg::BufferSource source(js, chunk);
                        controller->enqueue(js,
                            JSG_REQUIRE_NONNULL(decoder->decodePtr(js, source.asArrayPtr(), false),
                                TypeError, "Failed to decode input."));
                        return js.resolvedPromise();
                      })),
        .flush = jsg::Function<Transformer::FlushAlgorithm>(
            JSG_VISITABLE_LAMBDA((decoder = decoder.addRef()), (decoder),
                (jsg::Lock& js, auto controller) {
                  controller->enqueue(js,
                      JSG_REQUIRE_NONNULL(decoder->decodePtr(js, kj::ArrayPtr<kj::byte>(), true),
                          TypeError, "Failed to decode input."));
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
