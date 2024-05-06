// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "transform.h"
#include "standard.h"
#include "internal.h"
#include <workerd/io/features.h>
#include <workerd/jsg/function.h>

namespace workerd::api {

namespace {
    template <typename T>
jsg::Function<T> maybeAddFunctor(auto t) {
  if (IoContext::hasCurrent()) {
    return jsg::Function<T>(IoContext::current().addFunctor(kj::mv(t)));
  }
  return jsg::Function<T>(kj::mv(t));
}
}

jsg::Ref<TransformStream> TransformStream::constructor(
    jsg::Lock& js,
    jsg::Optional<Transformer> maybeTransformer,
    jsg::Optional<StreamQueuingStrategy> maybeWritableStrategy,
    jsg::Optional<StreamQueuingStrategy> maybeReadableStrategy) {

  if (FeatureFlags::get(js).getTransformStreamJavaScriptControllers()) {
    // The standard implementation. Here the TransformStream is backed by readable
    // and writable streams using the JavaScript-backed controllers. Data that is
    // written to the writable side passes through the transform function that is
    // given in maybeTransformer. If no transform function is given, then any value
    // written is passed through unchanged.
    //
    // Per the standard specification, any JavaScript value can be written to and
    // read from the transform stream, and the readable side does *not* support BYOB
    // reads.
    //
    // Persistent references to the TransformStreamDefaultController are held by both
    // the readable and writable sides. The actual TransformStream object can be dropped
    // and allowed to be garbage collected.

    auto controller = jsg::alloc<TransformStreamDefaultController>(js);
    auto transformer = kj::mv(maybeTransformer).orDefault({});

    // By default, let's signal backpressure on the readable side by setting the highWaterMark
    // to zero if a strategy is not given. This effectively means that writes/reads will be
    // one to one as long as the writer is respecting backpressure signals. If buffering
    // occurs, it will happen in the writable side of the transform stream.
    auto readableStrategy = kj::mv(maybeReadableStrategy).orDefault(StreamQueuingStrategy {
      .highWaterMark = 0,
    });

    auto readable = ReadableStream::constructor(
        js,
        UnderlyingSource {
          .type = kj::none,
          .autoAllocateChunkSize = kj::none,
          .start = maybeAddFunctor<UnderlyingSource::StartAlgorithm>(JSG_VISITABLE_LAMBDA(
                  (controller = controller.addRef()),
                  (controller),
                  (jsg::Lock& js, auto c) mutable {
            return controller->getStartPromise(js);
          })),
          .pull = maybeAddFunctor<UnderlyingSource::PullAlgorithm>(JSG_VISITABLE_LAMBDA(
                  (controller = controller.addRef()),
                  (controller),
                  (jsg::Lock& js, auto c) mutable {
            return controller->pull(js);
          })),
          .cancel = maybeAddFunctor<UnderlyingSource::CancelAlgorithm>(JSG_VISITABLE_LAMBDA(
                  (controller = controller.addRef()),
                  (controller),
                  (jsg::Lock& js, auto reason) mutable {
            return controller->cancel(js, reason);
          })),
          .expectedLength = transformer.expectedLength.map([](uint64_t expectedLength) {
            return expectedLength;
          }),
        },
        kj::mv(readableStrategy));

    auto writable = WritableStream::constructor(
        js,
        UnderlyingSink {
          .type = kj::none,
          .start = maybeAddFunctor<UnderlyingSink::StartAlgorithm>(JSG_VISITABLE_LAMBDA(
                  (controller = controller.addRef()),
                  (controller),
                  (jsg::Lock& js, auto c) mutable {
            return controller->getStartPromise(js);
          })),
          .write = maybeAddFunctor<UnderlyingSink::WriteAlgorithm>(JSG_VISITABLE_LAMBDA(
                  (controller = controller.addRef()),
                  (controller),
                  (jsg::Lock& js, auto chunk, auto c) mutable {
            return controller->write(js, chunk);
          })),
          .abort = maybeAddFunctor<UnderlyingSink::AbortAlgorithm>(JSG_VISITABLE_LAMBDA(
                  (controller = controller.addRef()),
                  (controller),
                  (jsg::Lock& js, auto reason) mutable {
            return controller->abort(js, reason);
          })),
          .close = maybeAddFunctor<UnderlyingSink::CloseAlgorithm>(JSG_VISITABLE_LAMBDA(
                  (controller = controller.addRef()),
                  (controller),
                  (jsg::Lock& js) mutable {
            return controller->close(js);
          })),
        },
        kj::mv(maybeWritableStrategy));

    // The controller will store c++ references to both the readable and writable
    // streams underlying controllers.
    controller->init(js, readable, writable, kj::mv(transformer));

    return jsg::alloc<TransformStream>(kj::mv(readable), kj::mv(writable));
  }

  // The old implementation just defers to IdentityTransformStream. If any of the arguments
  // are specified we throw because it's most likely that they want the standard implementation
  // but the compatibility flag is not set.
  if (maybeTransformer != kj::none ||
      maybeWritableStrategy != kj::none ||
      maybeReadableStrategy != kj::none) {
    IoContext::current().logWarningOnce(
        "To use the new TransformStream() constructor with a "
        "custom transformer, enable the transformstream_enable_standard_constructor compatibility flag. "
        "Refer to the docs for more information: https://developers.cloudflare.com/workers/platform/compatibility-dates/#compatibility-flags");
  }

  return IdentityTransformStream::constructor(js);
}

jsg::Ref<IdentityTransformStream> IdentityTransformStream::constructor(
    jsg::Lock& js,
    jsg::Optional<IdentityTransformStream::QueuingStrategy> maybeQueuingStrategy) {

  auto& ioContext = IoContext::current();
  auto pipe = newIdentityPipe();

  kj::Maybe<uint64_t> maybeHighWaterMark = kj::none;
  KJ_IF_SOME(queuingStrategy, maybeQueuingStrategy) {
    maybeHighWaterMark = queuingStrategy.highWaterMark;
  }

  return jsg::alloc<IdentityTransformStream>(
      jsg::alloc<ReadableStream>(ioContext, kj::mv(pipe.in)),
      jsg::alloc<WritableStream>(ioContext, kj::mv(pipe.out), maybeHighWaterMark));
}

jsg::Ref<FixedLengthStream> FixedLengthStream::constructor(
    jsg::Lock& js,
    uint64_t expectedLength,
    jsg::Optional<IdentityTransformStream::QueuingStrategy> maybeQueuingStrategy) {
  constexpr uint64_t MAX_SAFE_INTEGER = (1ull << 53) - 1;

  JSG_REQUIRE(expectedLength <= MAX_SAFE_INTEGER, TypeError,
      "FixedLengthStream requires an integer expected length less than 2^53.");

  auto& ioContext = IoContext::current();
  auto pipe = newIdentityPipe(uint64_t(expectedLength));

  kj::Maybe<uint64_t> maybeHighWaterMark = kj::none;
  // For a FixedLengthStream we do not want a highWaterMark higher than the expectedLength.
  KJ_IF_SOME(queuingStrategy, maybeQueuingStrategy) {
    maybeHighWaterMark = queuingStrategy.highWaterMark.map([&](uint64_t highWaterMark) {
      return kj::min(expectedLength, highWaterMark);
    });
  }

  return jsg::alloc<FixedLengthStream>(
      jsg::alloc<ReadableStream>(ioContext, kj::mv(pipe.in)),
      jsg::alloc<WritableStream>(ioContext, kj::mv(pipe.out), maybeHighWaterMark));
}

OneWayPipe newIdentityPipe(kj::Maybe<uint64_t> expectedLength) {
  auto readableSide = kj::refcounted<IdentityTransformStreamImpl>(expectedLength);
  auto writableSide = kj::addRef(*readableSide);
  return OneWayPipe {
    .in = kj::mv(readableSide),
    .out = kj::mv(writableSide)
  };
}

}  // namespace workerd::api
