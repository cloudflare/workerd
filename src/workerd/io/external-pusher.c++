// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/external-pusher.h>
#include <workerd/jsg/jsg.h>

namespace workerd {

// =======================================================================================
// ReadableStream handling

namespace {

// TODO(cleanup): These classes have been copied from streams/readable.c++. The copies there can be
//   deleted as soon as we've switched from StreamSink to ExternalPusher and can delete all the
//   StreamSink-related code. For now I'm not trying to avoid duplication.

// HACK: We need as async pipe, like kj::newOneWayPipe(), except supporting explicit end(). So we
//   wrap the two ends of the pipe in special adapters that track whether end() was called.
class ExplicitEndOutputPipeAdapter final: public capnp::ExplicitEndOutputStream {
 public:
  ExplicitEndOutputPipeAdapter(
      kj::Own<kj::AsyncOutputStream> inner, kj::Own<kj::RefcountedWrapper<bool>> ended)
      : inner(kj::mv(inner)),
        ended(kj::mv(ended)) {}

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return KJ_REQUIRE_NONNULL(inner)->write(buffer);
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return KJ_REQUIRE_NONNULL(inner)->write(pieces);
  }

  kj::Maybe<kj::Promise<uint64_t>> tryPumpFrom(
      kj::AsyncInputStream& input, uint64_t amount) override {
    return KJ_REQUIRE_NONNULL(inner)->tryPumpFrom(input, amount);
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return KJ_REQUIRE_NONNULL(inner)->whenWriteDisconnected();
  }

  kj::Promise<void> end() override {
    // Signal to the other side that end() was actually called.
    ended->getWrapped() = true;
    inner = kj::none;
    return kj::READY_NOW;
  }

 private:
  kj::Maybe<kj::Own<kj::AsyncOutputStream>> inner;
  kj::Own<kj::RefcountedWrapper<bool>> ended;
};

class ExplicitEndInputPipeAdapter final: public kj::AsyncInputStream {
 public:
  ExplicitEndInputPipeAdapter(kj::Own<kj::AsyncInputStream> inner,
      kj::Own<kj::RefcountedWrapper<bool>> ended,
      kj::Maybe<uint64_t> expectedLength)
      : inner(kj::mv(inner)),
        ended(kj::mv(ended)),
        expectedLength(expectedLength) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    size_t result = co_await inner->tryRead(buffer, minBytes, maxBytes);

    KJ_IF_SOME(l, expectedLength) {
      KJ_ASSERT(result <= l);
      l -= result;
      if (l == 0) {
        // If we got all the bytes we expected, we treat this as a successful end, because the
        // underlying KJ pipe is not actually going to wait for the other side to drop. This is
        // consistent with the behavior of Content-Length in HTTP anyway.
        ended->getWrapped() = true;
      }
    }

    if (result < minBytes) {
      // Verify that end() was called.
      if (!ended->getWrapped()) {
        JSG_FAIL_REQUIRE(Error, "ReadableStream received over RPC disconnected prematurely.");
      }
    }
    co_return result;
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return inner->tryGetLength();
  }

  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    return inner->pumpTo(output, amount);
  }

 private:
  kj::Own<kj::AsyncInputStream> inner;
  kj::Own<kj::RefcountedWrapper<bool>> ended;
  kj::Maybe<uint64_t> expectedLength;
};

}  // namespace

class ExternalPusherImpl::InputStreamImpl final: public ExternalPusher::InputStream::Server {
 public:
  InputStreamImpl(kj::Own<kj::AsyncInputStream> stream): stream(kj::mv(stream)) {}

  kj::Maybe<kj::Own<kj::AsyncInputStream>> stream;
};

kj::Promise<void> ExternalPusherImpl::pushByteStream(PushByteStreamContext context) {
  kj::Maybe<uint64_t> expectedLength;
  auto lp1 = context.getParams().getLengthPlusOne();
  if (lp1 > 0) {
    expectedLength = lp1 - 1;
  }

  auto pipe = kj::newOneWayPipe(expectedLength);

  auto endedFlag = kj::refcounted<kj::RefcountedWrapper<bool>>(false);

  auto out = kj::heap<ExplicitEndOutputPipeAdapter>(kj::mv(pipe.out), kj::addRef(*endedFlag));
  auto in =
      kj::heap<ExplicitEndInputPipeAdapter>(kj::mv(pipe.in), kj::mv(endedFlag), expectedLength);

  auto results = context.initResults(capnp::MessageSize{4, 2});

  results.setSource(inputStreamSet.add(kj::heap<InputStreamImpl>(kj::mv(in))));
  results.setSink(byteStreamFactory.kjToCapnp(kj::mv(out)));
  return kj::READY_NOW;
}

kj::Own<kj::AsyncInputStream> ExternalPusherImpl::unwrapStream(
    ExternalPusher::InputStream::Client cap) {
  auto& unwrapped = KJ_REQUIRE_NONNULL(
      inputStreamSet.tryGetLocalServerSync(cap), "pushed external is not a byte stream");

  return KJ_REQUIRE_NONNULL(kj::mv(kj::downcast<InputStreamImpl>(unwrapped).stream),
      "pushed byte stream has already been consumed");
}

// =======================================================================================
// AbortSignal handling

namespace {

// The jsrpc handler that receives aborts from the remote and triggers them locally
//
// TODO(cleanup): This class has been copied from external-pusher.c++. The copy there can be
//   deleted as soon as we've switched from StreamSink to ExternalPusher and can delete all the
//   StreamSink-related code. For now I'm not trying to avoid duplication.
class AbortTriggerRpcServer final: public rpc::AbortTrigger::Server {
 public:
  AbortTriggerRpcServer(kj::Own<kj::PromiseFulfiller<void>> fulfiller,
      kj::Own<ExternalPusherImpl::PendingAbortReason>&& pendingReason)
      : fulfiller(kj::mv(fulfiller)),
        pendingReason(kj::mv(pendingReason)) {}

  kj::Promise<void> abort(AbortContext abortCtx) override {
    auto params = abortCtx.getParams();
    auto reason = params.getReason().getV8Serialized();

    pendingReason->getWrapped() = kj::heapArray(reason.asBytes());
    fulfiller->fulfill();
    return kj::READY_NOW;
  }

  kj::Promise<void> release(ReleaseContext releaseCtx) override {
    released = true;
    return kj::READY_NOW;
  }

  ~AbortTriggerRpcServer() noexcept(false) {
    if (pendingReason->getWrapped() != nullptr) {
      // Already triggered
      return;
    }

    if (!released) {
      pendingReason->getWrapped() = JSG_KJ_EXCEPTION(FAILED, DOMAbortError,
          "An AbortSignal received over RPC was implicitly aborted because the connection back to "
          "its trigger was lost.");
    }

    // Always fulfill the promise in case the AbortSignal was waiting
    fulfiller->fulfill();
  }

 private:
  kj::Own<kj::PromiseFulfiller<void>> fulfiller;
  kj::Own<ExternalPusherImpl::PendingAbortReason> pendingReason;
  bool released = false;
};

}  // namespace

class ExternalPusherImpl::AbortSignalImpl final: public ExternalPusher::AbortSignal::Server {
 public:
  AbortSignalImpl(AbortSignal content): content(kj::mv(content)) {}

  kj::Maybe<AbortSignal> content;
};

kj::Promise<void> ExternalPusherImpl::pushAbortSignal(PushAbortSignalContext context) {
  auto paf = kj::newPromiseAndFulfiller<void>();
  auto pendingReason = kj::refcounted<PendingAbortReason>();

  auto results = context.initResults(capnp::MessageSize{4, 2});

  results.setTrigger(
      kj::heap<AbortTriggerRpcServer>(kj::mv(paf.fulfiller), kj::addRef(*pendingReason)));
  results.setSignal(abortSignalSet.add(
      kj::heap<AbortSignalImpl>(AbortSignal{kj::mv(paf.promise), kj::mv(pendingReason)})));

  return kj::READY_NOW;
}

ExternalPusherImpl::AbortSignal ExternalPusherImpl::unwrapAbortSignal(
    ExternalPusher::AbortSignal::Client cap) {
  auto& unwrapped = KJ_REQUIRE_NONNULL(
      abortSignalSet.tryGetLocalServerSync(cap), "pushed external is not an AbortSignal");

  return KJ_REQUIRE_NONNULL(kj::mv(kj::downcast<AbortSignalImpl>(unwrapped).content),
      "pushed AbortSignal has already been consumed");
}

}  // namespace workerd
