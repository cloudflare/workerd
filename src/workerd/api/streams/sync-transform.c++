// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sync-transform.h"

#include <workerd/io/io-context.h>
#include <workerd/util/ring-buffer.h>

namespace workerd::api {

namespace {

// Implements both sides of the stream. The readable side and the writable side each hold a
// reference, and every promise this returns holds another, so it lives until both sides and all
// of their operations are gone.
class SyncTransform final: public kj::Refcounted,
                           public ReadableStreamSource,
                           public WritableStreamSink {
 public:
  SyncTransform(
      kj::Own<SyncTransformStage> stage, size_t highWaterMark, jsg::ExternalMemoryAdjustment memory)
      : stage(kj::mv(stage)),
        highWaterMark(kj::max(highWaterMark, size_t(1))),
        memory(kj::mv(memory)),
        remainingLength(this->stage->getOutputLength()) {}

  // ReadableStreamSource ----------------------------------------------------------------

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    KJ_REQUIRE(minBytes <= maxBytes);
    KJ_REQUIRE(readWaiter == kj::none, "only one read may be pending at a time");
    if (maxBytes == 0) return size_t(0);
    // A read that returns no bytes means the end of the stream, so every read waits for one.
    minBytes = kj::max(minBytes, size_t(1));
    auto dest = kj::arrayPtr(static_cast<kj::byte*>(buffer), maxBytes);
    KJ_IF_SOME(n, tryReadNow(dest, minBytes)) {
      return n;
    }
    // Take what is queued now, so that the writer isn't held back by backpressure while the read
    // waits for the rest of its minimum. The wait is not I/O: the writer, which is, is what ends
    // it.
    size_t filled = dequeue(dest);
    return kj::newAdaptedPromise<size_t, ReadWaiter>(*this, dest, minBytes, filled)
        .attach(kj::addRef(*this));
  }

  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    KJ_REQUIRE(minBytes <= buffer.size());
    if (readWaiter != kj::none) return kj::none;
    if (buffer.size() == 0) return size_t(0);
    return tryReadNow(buffer, kj::max(minBytes, size_t(1)));
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    if (encoding != StreamEncoding::IDENTITY) return kj::none;
    KJ_IF_SOME(length, remainingLength) {
      // A consumer told that nothing remains would not read again, so until the stage has
      // ended successfully, and can no longer fail, report no length rather than zero.
      if (length == 0 && !succeeded()) return kj::none;
      return length;
    }
    return kj::none;
  }

  void cancel(kj::Exception reason) override {
    fail(kj::mv(reason));
  }

  // WritableStreamSink ------------------------------------------------------------------

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    // A coroutine, so that `buffer` lives in its frame for as long as writePieces() needs it.
    co_await writePieces(kj::arrayPtr(buffer));
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    return writePieces(pieces);
  }

  bool tryWriteSync(kj::ArrayPtr<const kj::byte> buffer) override {
    return tryWriteSync(kj::arrayPtr(buffer));
  }

  bool tryWriteSync(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    if (writeInProgress || ended || failure != kj::none || queuedBytes > highWaterMark) {
      return false;
    }
    // Only take what fits in one increment: a larger write goes through write(), which waits
    // for the reader between increments.
    size_t total = 0;
    for (auto piece: pieces) total += piece.size();
    if (total > highWaterMark) return false;
    for (auto piece: pieces) push(piece);
    return true;
  }

  kj::Promise<void> end() override {
    // A coroutine, so that failures are reported through the promise: callers don't expect end()
    // itself to throw.
    //
    // The pump that feeds this sink and the stream that owns it can both end it, so a second
    // end() is a no-op, as it is for other native sinks.
    if (ended && failure == kj::none) co_return;
    requireWritable();
    ended = true;
    runStage([&](SyncTransformStage::Output& output) { stage->end(output); });
    // The stage is done, so let go of what it holds.
    releaseStage();
  }

  void abort(kj::Exception reason) override {
    // A writable stream aborts its sink when a write fails. If the stage failed that write, the
    // output produced before the failure is still owed to the reader.
    if (failure != kj::none) return;
    fail(kj::mv(reason));
  }

 private:
  // The read waiting for output. Output is copied into its buffer as it arrives, and the read
  // is settled once it holds minBytes. Its destructor deregisters it, so a read that is canceled
  // has nothing more copied into its buffer.
  class ReadWaiter {
   public:
    ReadWaiter(kj::PromiseFulfiller<size_t>& fulfiller,
        SyncTransform& owner,
        kj::ArrayPtr<kj::byte> buffer,
        size_t minBytes,
        size_t filled)
        : fulfiller(fulfiller),
          owner(owner),
          buffer(buffer),
          minBytes(minBytes),
          filled(filled) {
      owner.readWaiter = *this;
    }
    ~ReadWaiter() noexcept(false) {
      KJ_IF_SOME(waiter, owner.readWaiter) {
        if (&waiter == this) {
          owner.readWaiter = kj::none;
          if (filled > 0) {
            // The bytes already copied into the canceled read are lost, so the output can't
            // continue intact.
            owner.fail(JSG_KJ_EXCEPTION(FAILED, Error, "A partially fulfilled read was canceled."));
          }
        }
      }
    }
    KJ_DISALLOW_COPY_AND_MOVE(ReadWaiter);

    kj::PromiseFulfiller<size_t>& fulfiller;
    SyncTransform& owner;
    kj::ArrayPtr<kj::byte> buffer;
    size_t minBytes;
    size_t filled;
  };

  // A write waiting for the reader to drain the output queue to the high-water mark.
  class WriteWaiter {
   public:
    WriteWaiter(kj::PromiseFulfiller<void>& fulfiller, SyncTransform& owner)
        : fulfiller(fulfiller),
          owner(owner) {
      owner.writeWaiter = *this;
    }
    ~WriteWaiter() noexcept(false) {
      KJ_IF_SOME(waiter, owner.writeWaiter) {
        if (&waiter == this) owner.writeWaiter = kj::none;
      }
    }
    KJ_DISALLOW_COPY_AND_MOVE(WriteWaiter);

    kj::PromiseFulfiller<void>& fulfiller;
    SyncTransform& owner;
  };

  // How the stream failed.
  struct Failure {
    kj::Exception exception;
    // True when the stage rejected its input: the output produced before that point is still
    // delivered, and readers see the error after it. Otherwise (the stream was canceled or
    // aborted) the output is discarded.
    bool afterOutput;
  };

  // Null once the stage has ended, failed, or been canceled: it is not used again.
  kj::Own<SyncTransformStage> stage;
  size_t highWaterMark;
  // Charges the queued output and the stage's retained bytes to the isolate.
  jsg::ExternalMemoryAdjustment memory;
  kj::Maybe<uint64_t> remainingLength;

  // Output waiting to be read. The first chunk has had `frontOffset` bytes read from it already.
  RingBuffer<kj::Array<kj::byte>, 8> queue;
  size_t frontOffset = 0;
  size_t queuedBytes = 0;

  bool ended = false;
  bool writeInProgress = false;
  kj::Maybe<Failure> failure;

  kj::Maybe<ReadWaiter&> readWaiter;
  kj::Maybe<WriteWaiter&> writeWaiter;

  // True once the stage has ended without failing, so that the output it queued is final.
  bool succeeded() const {
    return ended && failure == kj::none;
  }

  void requireWritable() {
    KJ_IF_SOME(f, failure) {
      kj::throwFatalException(f.exception.clone());
    }
    JSG_REQUIRE(!ended, TypeError, "This WritableStream has been closed.");
  }

  kj::Promise<void> writePieces(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) {
    auto self = kj::addRef(*this);
    requireWritable();
    KJ_REQUIRE(!writeInProgress, "only one write may be pending at a time");
    writeInProgress = true;
    KJ_DEFER(writeInProgress = false);
    // The caller keeps the pieces alive until the returned promise resolves.
    for (auto piece: pieces) {
      while (piece.size() > 0) {
        if (queuedBytes > highWaterMark) {
          // Wait for the reader to catch up before taking on more input.
          co_await kj::newAdaptedPromise<void, WriteWaiter>(*this);
          requireWritable();
        }
        auto increment = piece.first(kj::min(piece.size(), highWaterMark));
        piece = piece.slice(increment.size());
        push(increment);
      }
    }
  }

  void push(kj::ArrayPtr<const kj::byte> input) {
    runStage([&](SyncTransformStage::Output& output) { stage->push(input, output); });
  }

  // Runs one stage operation and queues what it produced. If the stage throws, the stream fails,
  // still delivering the output produced before the failure, and the exception propagates to the
  // writer.
  template <typename Func>
  void runStage(Func&& func) {
    SyncTransformStage::Output output;
    auto maybeException = kj::runCatchingExceptions([&]() { func(output); });
    for (auto& chunk: output) {
      if (chunk.size() == 0) continue;
      queuedBytes += chunk.size();
      queue.push_back(kj::mv(chunk));
    }
    KJ_IF_SOME(exception, maybeException) {
      failure = Failure{.exception = exception.clone(), .afterOutput = true};
      releaseStage();
      KJ_IF_SOME(waiter, writeWaiter) {
        writeWaiter = kj::none;
        waiter.fulfiller.reject(exception.clone());
      }
      serviceReader();
      kj::throwFatalException(kj::mv(exception));
    }
    updateMemory();
    serviceReader();
  }

  void releaseStage() {
    stage = nullptr;
    updateMemory();
  }

  void updateMemory() {
    // The chunk at the front of the queue is held in full until it has all been read.
    size_t retained = queuedBytes + frontOffset;
    if (stage.get() != nullptr) retained += stage->retainedBytes();
    memory.set(retained);
  }

  // The number of queued bytes a read may take now. When the output length is known, the last
  // byte is held back until the stage has ended successfully, so a consumer that stops reading
  // once it has the whole length still sees a failure the end of the input reveals.
  size_t deliverable() const {
    if (!succeeded()) {
      KJ_IF_SOME(length, remainingLength) {
        if (length <= queuedBytes) return length == 0 ? 0 : static_cast<size_t>(length - 1);
      }
    }
    return queuedBytes;
  }

  // Answers a read from the queue if it can be answered without waiting. `minBytes` is at least
  // 1.
  kj::Maybe<size_t> tryReadNow(kj::ArrayPtr<kj::byte> dest, size_t minBytes) {
    KJ_IF_SOME(f, failure) {
      // A read shorter than its minimum would be taken for the end of the stream, so once the
      // queue can't satisfy one, reads report the failure instead.
      if (!f.afterOutput || deliverable() < minBytes) {
        kj::throwFatalException(f.exception.clone());
      }
      return dequeue(dest);
    }
    if (deliverable() >= minBytes || succeeded()) {
      return dequeue(dest);
    }
    return kj::none;
  }

  size_t dequeue(kj::ArrayPtr<kj::byte> dest) {
    dest = dest.first(kj::min(dest.size(), deliverable()));
    size_t copied = 0;
    while (copied < dest.size() && !queue.empty()) {
      auto& front = queue.front();
      auto available = front.slice(frontOffset);
      size_t n = kj::min(available.size(), dest.size() - copied);
      dest.slice(copied).write(available.first(n));
      copied += n;
      frontOffset += n;
      if (frontOffset == front.size()) {
        queue.pop_front();
        frontOffset = 0;
      }
    }
    queuedBytes -= copied;
    KJ_IF_SOME(length, remainingLength) {
      remainingLength = length - kj::min(length, static_cast<uint64_t>(copied));
    }
    updateMemory();
    if (queuedBytes <= highWaterMark) {
      KJ_IF_SOME(waiter, writeWaiter) {
        writeWaiter = kj::none;
        waiter.fulfiller.fulfill();
      }
    }
    return copied;
  }

  // Copies newly queued output into the pending read, and settles the read once it holds its
  // minimum, or no more output can come. A settled read is deregistered at once, so that nothing
  // more is copied into its buffer.
  void serviceReader() {
    KJ_IF_SOME(waiter, readWaiter) {
      KJ_IF_SOME(f, failure) {
        if (f.afterOutput) waiter.filled += dequeue(waiter.buffer.slice(waiter.filled));
        readWaiter = kj::none;
        if (waiter.filled >= waiter.minBytes) {
          // Deliver what the read holds; the next read reports the failure.
          waiter.fulfiller.fulfill(kj::cp(waiter.filled));
        } else {
          // A short read would be taken for the end of the stream.
          waiter.fulfiller.reject(f.exception.clone());
        }
        return;
      }
      waiter.filled += dequeue(waiter.buffer.slice(waiter.filled));
      if (waiter.filled >= waiter.minBytes || succeeded()) {
        readWaiter = kj::none;
        waiter.fulfiller.fulfill(kj::cp(waiter.filled));
      }
    }
  }

  // Cancels the stream from either side, discarding any output not yet read.
  void fail(kj::Exception reason) {
    KJ_IF_SOME(f, failure) {
      // Keep the first error, which later reads and writes report.
      f.afterOutput = false;
    } else {
      failure = Failure{.exception = reason.clone(), .afterOutput = false};
      if (stage.get() != nullptr) stage->cancel(reason);
    }
    stage = nullptr;
    queue.clear();
    frontOffset = 0;
    queuedBytes = 0;
    updateMemory();
    KJ_IF_SOME(waiter, readWaiter) {
      readWaiter = kj::none;
      waiter.fulfiller.reject(reason.clone());
    }
    KJ_IF_SOME(waiter, writeWaiter) {
      writeWaiter = kj::none;
      waiter.fulfiller.reject(kj::mv(reason));
    }
  }
};

}  // namespace

SyncTransformStreams newSyncTransformStreams(
    jsg::Lock& js, IoContext& ioContext, kj::Own<SyncTransformStage> stage, size_t highWaterMark) {
  auto transform = kj::refcounted<SyncTransform>(
      kj::mv(stage), highWaterMark, js.getExternalMemoryAdjustment(0));
  kj::Own<ReadableStreamSource> source = kj::addRef(*transform);
  kj::Own<WritableStreamSink> sink = kj::mv(transform);
  return SyncTransformStreams{
    .readable = JsReadableStream::create(js, ioContext, kj::mv(source)),
    .writable = JsWritableStream::create(
        js, ioContext, kj::mv(sink), ioContext.getMetrics().tryCreateWritableByteStreamObserver()),
  };
}

}  // namespace workerd::api
