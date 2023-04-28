// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "transform.h"
#include "standard.h"
#include <workerd/jsg/function.h>
#include <cmath>

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
    jsg::Optional<StreamQueuingStrategy> maybeReadableStrategy,
    CompatibilityFlags::Reader flags) {

  if (flags.getTransformStreamJavaScriptControllers()) {
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

    auto readable = ReadableStream::constructor(
        js,
        UnderlyingSource {
          .type = nullptr,
          .autoAllocateChunkSize = nullptr,
          .start = maybeAddFunctor<UnderlyingSource::StartAlgorithm>(JSG_VISITABLE_LAMBDA(
                  (controller = controller.addRef()),
                  (controller),
                  (jsg::Lock& js, auto c) mutable {
            return controller->getStartPromise();
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
        },
        kj::mv(maybeReadableStrategy),
        kj::cp(flags));

    auto writable = WritableStream::constructor(
        js,
        UnderlyingSink {
          .type = nullptr,
          .start = maybeAddFunctor<UnderlyingSink::StartAlgorithm>(JSG_VISITABLE_LAMBDA(
                  (controller = controller.addRef()),
                  (controller),
                  (jsg::Lock& js, auto c) mutable {
            return controller->getStartPromise();
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
        kj::mv(maybeWritableStrategy),
        kj::mv(flags));

    // The controller will store c++ references to both the readable and writable
    // streams underlying controllers.
    controller->init(js, readable, writable, kj::mv(maybeTransformer));

    return jsg::alloc<TransformStream>(kj::mv(readable), kj::mv(writable));
  }

  // The old implementation just defers to IdentityTransformStream. If any of the arguments
  // are specified we throw because it's most likely that they want the standard implementation
  // but the feature flag is not set.
  if (maybeTransformer != nullptr ||
      maybeWritableStrategy != nullptr ||
      maybeReadableStrategy != nullptr) {
    IoContext::current().logWarningOnce(
        "To use the new TransformStream() constructor with a "
        "custom transformer, enable the transformstream_enable_standard_constructor feature flag.");
  }

  return IdentityTransformStream::constructor(js);
}

jsg::Ref<IdentityTransformStream> IdentityTransformStream::constructor(
    jsg::Lock& js,
    jsg::Optional<IdentityTransformStream::QueuingStrategy> maybeQueuingStrategy) {
  auto readableSide = kj::refcounted<IdentityTransformStreamImpl>();
  auto writableSide = kj::addRef(*readableSide);

  auto& ioContext = IoContext::current();

  kj::Maybe<uint64_t> maybeHighWaterMark = nullptr;
  KJ_IF_MAYBE(queuingStrategy, maybeQueuingStrategy) {
    maybeHighWaterMark = queuingStrategy->highWaterMark;
  }

  return jsg::alloc<IdentityTransformStream>(
      jsg::alloc<ReadableStream>(ioContext, kj::mv(readableSide)),
      jsg::alloc<WritableStream>(ioContext, kj::mv(writableSide), maybeHighWaterMark));
}

jsg::Ref<FixedLengthStream> FixedLengthStream::constructor(
    jsg::Lock& js,
    uint64_t expectedLength,
    jsg::Optional<IdentityTransformStream::QueuingStrategy> maybeQueuingStrategy) {
  constexpr uint64_t MAX_SAFE_INTEGER = (1ull << 53) - 1;

  JSG_REQUIRE(expectedLength <= MAX_SAFE_INTEGER, TypeError,
      "FixedLengthStream requires an integer expected length less than 2^53.");

  auto readableSide = kj::refcounted<IdentityTransformStreamImpl>(uint64_t(expectedLength));
  auto writableSide = kj::addRef(*readableSide);

  auto& ioContext = IoContext::current();

  kj::Maybe<uint64_t> maybeHighWaterMark = nullptr;
  // For a FixedLengthStream we do not want a highWaterMark higher than the expectedLength.
  KJ_IF_MAYBE(queuingStrategy, maybeQueuingStrategy) {
    maybeHighWaterMark = queuingStrategy->highWaterMark.map([&](uint64_t highWaterMark) {
      return kj::min(expectedLength, highWaterMark);
    });
  }

  return jsg::alloc<FixedLengthStream>(
      jsg::alloc<ReadableStream>(ioContext, kj::mv(readableSide)),
      jsg::alloc<WritableStream>(ioContext, kj::mv(writableSide), maybeHighWaterMark));
}

}  // namespace workerd::api
