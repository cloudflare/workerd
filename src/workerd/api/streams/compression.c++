// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "compression.h"

#include <workerd/io/features.h>
#include <workerd/util/ring-buffer.h>
#include <workerd/util/state-machine.h>

namespace workerd::api {

namespace {

// The legacy async frontend: adapts the synchronous CodecStage (api/compression.h) to the
// promise-based AsyncInputStream + ExplicitEndOutputStream interfaces consumed by the
// internal streams machinery. Owns ALL of the asynchrony — the pending-read ring, the
// canceler, and the lifecycle state machine; the codec work itself lives entirely in the
// stage.
class CompressionStreamImpl final: public kj::Refcounted,
                                   public kj::AsyncInputStream,
                                   public capnp::ExplicitEndOutputStream {
 public:
  explicit CompressionStreamImpl(CodecStage::Mode mode,
      kj::String format,
      CodecStage::Flags flags,
      kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
      : stage(mode, format, flags, kj::mv(externalMemoryTarget)),
        state(decltype(state)::create<Open>()) {}

  // WritableStreamSink implementation ---------------------------------------------------

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    requireActive("Write after close");
    runCodec([&]() { stage.push(buffer); });
    maybeFulfillRead();
    co_return;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    // We check state here so that we catch errors even if pieces is empty.
    requireActive("Write after close");
    for (auto piece: pieces) {
      co_await write(piece);
    }
    co_return;
  }

  bool tryWriteSync(kj::ArrayPtr<const byte> buffer) override {
    // Writes never involve async I/O: they compress into the stage's internal output buffer
    // (there is currently no backpressure), so any write in the active state can complete
    // synchronously.
    if (isInTerminalState()) {
      // Closed or errored; let the async path surface the appropriate exception.
      return false;
    }
    // Note: the codec may throw if the compression itself fails, exactly as the async write()
    // would; runCodec() applies the same teardown either way.
    runCodec([&]() { stage.push(buffer); });
    maybeFulfillRead();
    return true;
  }

  bool tryWriteSync(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    if (isInTerminalState()) {
      // Closed or errored; let the async path surface the appropriate exception.
      return false;
    }
    for (auto piece: pieces) {
      runCodec([&]() { stage.push(piece); });
      maybeFulfillRead();
    }
    return true;
  }

  kj::Promise<void> end() override {
    transitionToEnded();
    runCodec([&]() { stage.end(); });
    maybeFulfillRead();
    co_return;
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

  void abortWrite(kj::Exception&& reason) override {
    cancelInternal(kj::mv(reason));
  }

  // AsyncInputStream implementation -----------------------------------------------------

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    KJ_ASSERT(minBytes <= maxBytes);
    // Re-throw any stored exception
    throwIfException();
    // If stream has ended normally and no buffered data, return EOF
    if (isInTerminalState() && stage.empty()) {
      co_return static_cast<size_t>(0);
    }
    // Active or terminal with data remaining
    co_return co_await tryReadInternal(
        kj::arrayPtr(reinterpret_cast<kj::byte*>(buffer), maxBytes), minBytes);
  }

  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    KJ_REQUIRE(minBytes <= buffer.size());
    // Re-throw any stored exception: a synchronous answer exists, and it is this error, exactly
    // as tryRead() would report it.
    throwIfException();

    // Preserve FIFO ordering: if asynchronous reads are already waiting for data, we must not
    // serve a synchronous read ahead of them.
    if (!pendingReads.empty()) {
      return kj::none;
    }

    if (stage.available() >= minBytes || isInTerminalState()) {
      // Serve directly from the stage's buffered output. (In the terminal state this may copy
      // fewer than minBytes -- possibly zero -- which correctly signals EOF, matching tryRead().)
      return stage.pull(buffer);
    }

    // Not enough data buffered; the read would have to wait for a future write.
    return kj::none;
  }

 private:
  struct PendingRead {
    kj::ArrayPtr<kj::byte> buffer;
    size_t minBytes = 1;
    size_t filled = 0;
    kj::Own<kj::PromiseFulfiller<size_t>> promise;
  };

  // Runs a stage operation, translating a codec exception into stream teardown (reject
  // pending reads, error the state machine) before rethrowing — preserving the error path
  // of the previously fused pump/state code.
  template <typename Func>
  void runCodec(Func&& func) {
    KJ_IF_SOME(exception, kj::runCatchingExceptions(kj::fwd<Func>(func))) {
      cancelInternal(exception.clone());
      kj::throwFatalException(kj::mv(exception));
    }
  }

  void cancelInternal(kj::Exception reason) {
    stage.clear();

    while (!pendingReads.empty()) {
      auto pending = kj::mv(pendingReads.front());
      pendingReads.pop_front();
      if (pending.promise->isWaiting()) {
        pending.promise->reject(reason.clone());
      }
    }

    canceler.cancel(reason.clone());
    transitionToErrored(kj::mv(reason));
  }

  kj::Promise<size_t> tryReadInternal(kj::ArrayPtr<kj::byte> dest, size_t minBytes) {
    // TODO(later): This does not yet implement any backpressure. A caller can keep calling
    // write without reading, which will continue to fill the stage's internal buffer.
    //
    // If the stage currently buffers >= minBytes, then we'll fulfill the read immediately,
    // removing as many bytes as possible from the output queue.
    // If we reached the end (terminal state), resolve the read immediately as well, since
    // no new data is expected.
    if (stage.available() >= minBytes || isInTerminalState()) {
      co_return stage.pull(dest);
    }

    // Otherwise, create a pending read.
    auto promise = kj::newPromiseAndFulfiller<size_t>();
    auto pendingRead = PendingRead{
      .buffer = dest,
      .minBytes = minBytes,
      .filled = 0,
      .promise = kj::mv(promise.fulfiller),
    };

    // If there are any bytes queued, copy as much as possible into the buffer.
    if (stage.available() > 0) {
      pendingRead.filled = stage.pull(dest);
    }

    pendingReads.push_back(kj::mv(pendingRead));

    co_return co_await canceler.wrap(kj::mv(promise.promise));
  }

  // Fulfill as many pending reads as we can from the stage's buffered output.
  void maybeFulfillRead() {
    // If there are pending reads and data to be read, we'll loop through the pending reads
    // and fulfill them as much as possible.
    while (!pendingReads.empty() && stage.available() > 0) {
      auto& pending = pendingReads.front();

      if (!pending.promise->isWaiting()) {
        // The pending read was canceled!
        // Importantly, the pending.buffer is no longer valid here so we definitely want to
        // make sure we don't try to write anything to it!

        // If the pending read was already partially fulfilled, then we have a problem!
        // We can't just cancel and continue because the partially read data will be lost
        // so we need to report an error here and error the stream.
        if (pending.filled > 0) {
          auto ex = JSG_KJ_EXCEPTION(FAILED, Error, "A partially fulfilled read was canceled.");
          cancelInternal(ex.clone());
          kj::throwFatalException(kj::mv(ex));
        }

        auto ex = JSG_KJ_EXCEPTION(FAILED, Error, "The pending read was canceled.");
        cancelInternal(ex.clone());
        kj::throwFatalException(kj::mv(ex));
      }

      // The pending read is still viable so copy in as much as we can.
      pending.filled += stage.pull(pending.buffer.slice(pending.filled, pending.buffer.size()));

      // If we've met the minimum bytes requirement for the pending read, fulfill the read
      // promise.
      if (pending.filled >= pending.minBytes) {
        auto p = kj::mv(pending);
        pendingReads.pop_front();
        p.promise->fulfill(kj::mv(p.filled));
        continue;
      }

      // If we reached this point in the loop, remaining must be 0 so that we don't keep
      // iterating through on the same pending read.
      KJ_ASSERT(stage.empty());
    }

    if (isInTerminalState() && !pendingReads.empty()) {
      // We are ended and we have pending reads. Because of the loop above, one of either
      // pendingReads or the stage buffer must be empty, so if we got this far, stage.empty()
      // must be true. Let's check.
      KJ_ASSERT(stage.empty());
      // We need to flush any remaining reads.
      while (!pendingReads.empty()) {
        auto pending = kj::mv(pendingReads.front());
        pendingReads.pop_front();
        if (pending.promise->isWaiting()) {
          // Fulfill the pending read promise only if it hasn't already been canceled.
          pending.promise->fulfill(kj::mv(pending.filled));
        }
      }
    }
  }

  // Lifecycle -----------------------------------------------------------------------------

  void requireActive(kj::StringPtr errorMessage) {
    KJ_IF_SOME(exception, state.tryGetErrorUnsafe()) {
      kj::throwFatalException(exception.clone());
    }
    // isActive() returns true only if in Open state (the ActiveState)
    JSG_REQUIRE(state.isActive(), Error, errorMessage);
  }

  void transitionToEnded() {
    // If already in a terminal state (Ended or Exception), this is a no-op, preserving the
    // historical allowance for multiple end() calls.
    if (state.isTerminal()) return;
    auto result = state.transitionFromTo<Open, Ended>();
    KJ_REQUIRE(result != kj::none, "Stream already ended or errored");
  }

  void transitionToErrored(kj::Exception&& reason) {
    // Use forceTransitionTo because cancelInternal may be called when already in an error
    // state (e.g., from the runCodec error handling).
    state.forceTransitionTo<kj::Exception>(kj::mv(reason));
  }

  void throwIfException() {
    KJ_IF_SOME(exception, state.tryGetErrorUnsafe()) {
      kj::throwFatalException(exception.clone());
    }
  }

  bool isInTerminalState() {
    return state.isTerminal();
  }

  struct Ended {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "ended"_kj;
  };
  struct Open {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "open"_kj;
  };

  CodecStage stage;

  // State machine for tracking compression stream lifecycle:
  //   Open -> Ended (normal close via end())
  //   Open -> kj::Exception (error via abortWrite())
  // Ended is terminal, kj::Exception is implicitly terminal via ErrorState.
  StateMachine<TerminalStates<Ended>,
      ErrorState<kj::Exception>,
      ActiveState<Open>,
      Open,
      Ended,
      kj::Exception>
      state;

  kj::Canceler canceler;
  RingBuffer<PendingRead, 8> pendingReads;
};

// Adapter to bridge CompressionStreamImpl (which implements AsyncInputStream and
// ExplicitEndOutputStream) to the ReadableStreamSource/WritableStreamSink interfaces.
// TODO(soon): This class is intended to be replaced by the new ReadableSource/WritableSink
// interfaces once fully implemented. We will need an adapter that knows how to handle both
// sides of the stream once fully implemented. The current implementation in
// system-streams.c++ implements separate adapters for each side that are not aware of each
// other, making it unsuitable for this specific case.
class CompressionStreamAdapter final: public kj::Refcounted,
                                      public ReadableStreamSource,
                                      public WritableStreamSink {
 public:
  explicit CompressionStreamAdapter(kj::Rc<CompressionStreamImpl> impl)
      : impl(kj::mv(impl)),
        ioContext(IoContext::current()) {}

  // ReadableStreamSource implementation
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return impl->tryRead(buffer, minBytes, maxBytes).attach(ioContext.registerPendingEvent());
  }

  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    // No pending event registration is needed since a synchronous read never suspends.
    return impl->tryReadSync(buffer, minBytes);
  }

  void cancel(kj::Exception reason) override {
    // AsyncInputStream doesn't have cancel, but we can abort the write side
    impl->abortWrite(kj::mv(reason));
  }

  // WritableStreamSink implementation
  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return impl->write(buffer).attach(ioContext.registerPendingEvent());
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return impl->write(pieces).attach(ioContext.registerPendingEvent());
  }

  bool tryWriteSync(kj::ArrayPtr<const byte> buffer) override {
    // No pending event registration is needed since a synchronous write never suspends.
    return impl->tryWriteSync(buffer);
  }

  bool tryWriteSync(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return impl->tryWriteSync(pieces);
  }

  kj::Promise<void> end() override {
    return impl->end().attach(ioContext.registerPendingEvent());
  }

  void abort(kj::Exception reason) override {
    impl->abortWrite(kj::mv(reason));
  }

 private:
  kj::Rc<CompressionStreamImpl> impl;
  IoContext& ioContext;
};

}  // namespace

jsg::Ref<CompressionStream> CompressionStream::constructor(jsg::Lock& js, kj::String format) {
  JSG_REQUIRE(format == "deflate" || format == "gzip" || format == "deflate-raw", TypeError,
      "The compression format must be either 'deflate', 'deflate-raw' or 'gzip'.");

  auto impl = kj::rc<CompressionStreamImpl>(CodecStage::Mode::COMPRESS, kj::mv(format),
      CodecStage::Flags::NONE, js.getExternalMemoryTarget());

  auto& ioContext = IoContext::current();

  // Create a single adapter that implements both readable and writable sides
  auto adapter = kj::refcounted<CompressionStreamAdapter>(kj::mv(impl));
  auto readableSide = kj::addRef(*adapter);
  auto writableSide = kj::mv(adapter);

  return js.alloc<CompressionStream>(js.alloc<ReadableStream>(ioContext, kj::mv(readableSide)),
      js.alloc<WritableStream>(ioContext, kj::mv(writableSide),
          ioContext.getMetrics().tryCreateWritableByteStreamObserver()));
}

jsg::Ref<DecompressionStream> DecompressionStream::constructor(jsg::Lock& js, kj::String format) {
  JSG_REQUIRE(format == "deflate" || format == "gzip" || format == "deflate-raw", TypeError,
      "The compression format must be either 'deflate', 'deflate-raw' or 'gzip'.");

  auto impl = kj::rc<CompressionStreamImpl>(CodecStage::Mode::DECOMPRESS, kj::mv(format),
      FeatureFlags::get(js).getStrictCompression() ? CodecStage::Flags::STRICT
                                                   : CodecStage::Flags::NONE,
      js.getExternalMemoryTarget());

  auto& ioContext = IoContext::current();

  // Create a single adapter that implements both readable and writable sides
  auto adapter = kj::refcounted<CompressionStreamAdapter>(kj::mv(impl));
  auto readableSide = kj::addRef(*adapter);
  auto writableSide = kj::mv(adapter);

  return js.alloc<DecompressionStream>(js.alloc<ReadableStream>(ioContext, kj::mv(readableSide)),
      js.alloc<WritableStream>(ioContext, kj::mv(writableSide),
          ioContext.getMetrics().tryCreateWritableByteStreamObserver()));
}

}  // namespace workerd::api
