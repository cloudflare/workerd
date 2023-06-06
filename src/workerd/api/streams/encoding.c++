// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "encoding.h"
#include "standard.h"
#include "../encoding.h"
#include <workerd/jsg/buffersource.h>

namespace workerd::api {

namespace {
class TextEncoderStreamController: public kj::Refcounted {
public:
private:
  jsg::Ref<TextEncoder> encoder = jsg::alloc<TextEncoder>();
};
}  // namespace

jsg::Ref<TextEncoderStream> TextEncoderStream::constructor(jsg::Lock& js) {
  auto transformer = TransformStream::constructor(
    js,
    Transformer {
      .transform = jsg::Function<Transformer::TransformAlgorithm>(
          [](jsg::Lock& js, auto chunk, auto controller) {
      auto str = jsg::check(chunk->ToString(js.v8Context()));
      auto maybeBuffer = v8::ArrayBuffer::MaybeNew(js.v8Isolate, str->Utf8Length(js.v8Isolate));
      JSG_ASSERT(!maybeBuffer.IsEmpty(), RangeError,
                  "Cannot allocate space for TextEncoder.encode");
      auto buffer = maybeBuffer.ToLocalChecked();

      auto bytes = jsg::asBytes(buffer).releaseAsChars();
      [[maybe_unused]] int read = 0;
      [[maybe_unused]] auto written = str->WriteUtf8(
          js.v8Isolate,
          bytes.begin(),
          bytes.size(),
          &read,
          v8::String::NO_NULL_TERMINATION | v8::String::REPLACE_INVALID_UTF8);

      KJ_DASSERT(written == buffer->ByteLength());
      KJ_DASSERT(read == str->Length());
      controller->enqueue(js, v8::Uint8Array::New(buffer, 0, buffer->ByteLength()));
      return js.resolvedPromise();
    })},
    StreamQueuingStrategy {},
    StreamQueuingStrategy {});

  return jsg::alloc<TextEncoderStream>(transformer->getReadable(), transformer->getWritable());
}

jsg::Ref<TextDecoderStream> TextDecoderStream::constructor(
    jsg::Lock& js,
    jsg::Optional<kj::String> label,
    jsg::Optional<TextDecoderStreamInit> options) {
  auto decoder = TextDecoder::constructor(kj::mv(label), options.map([](auto& opts) {
    return TextDecoder::ConstructorOptions {
      .fatal = opts.fatal.orDefault(true),
    };
  }));

  // The controller will store c++ references to both the readable and writable
  // streams underlying controllers.
  auto transformer = TransformStream::constructor(
    js,
    Transformer {
      .transform = jsg::Function<Transformer::TransformAlgorithm>(
          JSG_VISITABLE_LAMBDA((decoder = decoder.addRef()),
                                (decoder),
                                (jsg::Lock& js, auto chunk, auto controller) {
        jsg::BufferSource source(js, chunk);
        controller->enqueue(js, JSG_REQUIRE_NONNULL(
            decoder->decodePtr(js.v8Isolate, source.asArrayPtr(), false),
            TypeError,
            "Failed to decode input."));
        return js.resolvedPromise();
      })),
      .flush = jsg::Function<Transformer::FlushAlgorithm>(
          JSG_VISITABLE_LAMBDA((decoder = kj::mv(decoder)),
                                (decoder),
                                (jsg::Lock& js, auto controller) {
        controller->enqueue(js, JSG_REQUIRE_NONNULL(
            decoder->decodePtr(js.v8Isolate, kj::ArrayPtr<kj::byte>(), true),
            TypeError,
            "Failed to decode input."));
        return js.resolvedPromise();
      }))
    },
    StreamQueuingStrategy {},
    StreamQueuingStrategy {});

  return jsg::alloc<TextDecoderStream>(transformer->getReadable(), transformer->getWritable());
}

}  // namespace workerd::api
