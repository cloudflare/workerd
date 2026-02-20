// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "internal.h"

#include "identity-transform-stream.h"
#include "readable.h"
#include "writable.h"

#include <workerd/api/util.h>
#include <workerd/io/features.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/string-buffer.h>

#include <kj/vector.h>

namespace workerd::api {

namespace {
// Use this in places where the exception thrown would cause finalizers to run. Your exception
// will not go anywhere, but we'll log the exception message to the console until the problem this
// papers over is fixed.
[[noreturn]] void throwTypeErrorAndConsoleWarn(kj::StringPtr message) {
  KJ_IF_SOME(context, IoContext::tryCurrent()) {
    if (context.hasWarningHandler()) {
      context.logWarning(message);
    }
  }

  kj::throwFatalException(kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
      kj::str(JSG_EXCEPTION(TypeError) ": ", message)));
}

kj::Promise<void> pumpTo(ReadableStreamSource& input, WritableStreamSink& output, bool end) {
  kj::byte buffer[65536]{};

  while (true) {
    auto amount = co_await input.tryRead(buffer, 1, kj::size(buffer));

    if (amount == 0) {
      if (end) {
        co_await output.end();
      }
      co_return;
    }

    co_await output.write(kj::arrayPtr(buffer, amount));
  }
}

// Modified from AllReader in kj/async-io.c++.
class AllReader final {
 public:
  explicit AllReader(ReadableStreamSource& input, uint64_t limit): input(input), limit(limit) {
    JSG_REQUIRE(limit > 0, TypeError, "Memory limit exceeded before EOF.");
    KJ_IF_SOME(length, input.tryGetLength(StreamEncoding::IDENTITY)) {
      // Oh hey, we might be able to bail early.
      JSG_REQUIRE(length < limit, TypeError, "Memory limit would be exceeded before EOF.");
    }
  }
  KJ_DISALLOW_COPY_AND_MOVE(AllReader);

  kj::Promise<kj::Array<kj::byte>> readAllBytes() {
    return read<kj::byte>();
  }

  kj::Promise<kj::String> readAllText(
      ReadAllTextOption option = ReadAllTextOption::NULL_TERMINATE) {
    auto data = co_await read<char>(option);
    co_return kj::String(kj::mv(data));
  }

 private:
  ReadableStreamSource& input;
  uint64_t limit;

  template <typename T>
  kj::Promise<kj::Array<T>> read(ReadAllTextOption option = ReadAllTextOption::NONE) {
    // There are a few complexities in this operation that make it difficult to completely
    // optimize. The most important is that even if a stream reports an expected length
    // using tryGetLength, we really don't know how much data the stream will produce until
    // we try to read it. The only signal we have that the stream is done producing data
    // is a zero-length result from tryRead. Unfortunately, we have to allocate a buffer
    // in advance of calling tryRead so we have to guess a bit at the size of the buffer
    // to allocate.
    //
    // In the previous implementation of this method, we would just blindly allocate a
    // 4096 byte buffer on every allocation, limiting each read iteration to a maximum
    // of 4096 bytes. This works fine for streams producing a small amount of data but
    // risks requiring a greater number of loop iterations and small allocations for streams
    // that produce larger amounts of data. Also in the previous implementation, every
    // loop iteration would allocate a new buffer regardless of how much of the previous
    // allocation was actually used -- so a stream that produces only 4000 bytes total
    // but only provides 10 bytes per iteration would end up with 400 reads and 400 4096
    // byte allocations. Doh! Fortunately our stream implementations tend to be a bit
    // smarter than that but it's still a worst case possibility that it's likely better
    // to avoid.
    //
    // So this implementation does things a bit differently.
    // First, we check to see if the stream can give an estimate on how much data it
    // expects to produce. If that length is within a given threshold, then best case
    // is we can perform the entire read with at most two allocations and two calls to
    // tryRead. The first allocation will be for the entire expected size of the stream,
    // which the first tryRead will attempt to fulfill completely. In the best case the
    // stream provides all of the data. The next allocation would be smaller and would
    // end up resulting in a zero-length read signaling that we are done. Hooray!
    //
    // Not everything can be best case scenario tho, unfortunately. If our first tryRead
    // does not fully consume the stream or fully fill the destination buffer, we're
    // going to need to try again. It is possible that the new allocation in the next
    // iteration will be wasted if the stream doesn't have any more data so it's important
    // for us to try to be conservative with the allocation. If the running total of data
    // we've seen so far is equal to or greater than the expected total length of the stream,
    // then the most likely case is that the next read will be zero-length -- but unfortunately
    // we can't know for sure! So for this we will fall back to a more conservative allocation
    // which is either 4096 bytes or the calculated amountToRead, whichever is the lower number.

    kj::Vector<kj::Array<T>> parts;
    uint64_t runningTotal = 0;
    static constexpr uint64_t MIN_BUFFER_CHUNK = 1024;
    static constexpr uint64_t DEFAULT_BUFFER_CHUNK = 4096;
    static constexpr uint64_t MAX_BUFFER_CHUNK = DEFAULT_BUFFER_CHUNK * 4;

    // If we know in advance how much data we'll be reading, then we can attempt to
    // optimize the loop here by setting the value specifically so we are only
    // allocating at most twice. But, to be safe, let's enforce an upper bound on each
    // allocation even if we do know the total.
    kj::Maybe<uint64_t> maybeLength = input.tryGetLength(StreamEncoding::IDENTITY);

    // The amountToRead is the regular allocation size we'll use right up until we've
    // read the number of expected bytes (if known). This number is calculated as the
    // minimum of (limit, MAX_BUFFER_CHUNK, maybeLength or DEFAULT_BUFFER_CHUNK). In
    // the best case scenario, this number is calculated such that we can read the
    // entire stream in one go if the amount of data is small enough and the stream
    // is well behaved.
    // If the stream does report a length, once we've read that number of bytes, we'll
    // fallback to the conservativeAllocation.
    uint64_t amountToRead =
        kj::min(limit, kj::min(MAX_BUFFER_CHUNK, maybeLength.orDefault(DEFAULT_BUFFER_CHUNK)));
    // amountToRead can be zero if the stream reported a zero-length. While the stream could
    // be lying about its length, let's skip reading anything in this case.
    if (amountToRead > 0) {
      for (;;) {
        auto bytes = kj::heapArray<T>(amountToRead);
        // Note that we're passing amountToRead as the *minBytes* here so the tryRead should
        // attempt to fill the entire buffer. If it doesn't, the implication is that we read
        // everything.
        uint64_t amount = co_await input.tryRead(bytes.begin(), amountToRead, amountToRead);
        KJ_DASSERT(amount <= amountToRead);

        runningTotal += amount;
        JSG_REQUIRE(runningTotal < limit, TypeError, "Memory limit exceeded before EOF.");

        if (amount < amountToRead) {
          // The stream has indicated that we're all done by returning a value less than the
          // full buffer length.
          // It is possible/likely that at least some amount of data was written to the buffer.
          // In which case we want to add that subset to the parts list here before we exit
          // the loop.
          if (amount > 0) {
            parts.add(bytes.first(amount).attach(kj::mv(bytes)));
          }
          break;
        }

        // Because we specify minSize equal to maxSize in the tryRead above, we should only
        // get here if the buffer was completely filled by the read. If it wasn't completely
        // filled, that is an indication that the stream is complete which is handled above.
        KJ_DASSERT(amount == bytes.size());
        parts.add(kj::mv(bytes));

        // If the stream provided an expected length and our running total is equal to
        // or greater than that length then we assume we're done.
        KJ_IF_SOME(length, maybeLength) {
          if (runningTotal >= length) {
            // We've read everything we expect to read but some streams need to be read
            // completely in order to properly finish and other streams might lie (although
            // they shouldn't). Sigh. So we're going to make the next allocation potentially
            // smaller and keep reading until we get a zero length. In the best case, the next
            // read is going to be zero length but we have to try which will require at least
            // one additional (potentially wasted) allocation. (If we don't there are multiple
            // test failures).
            amountToRead = kj::min(MIN_BUFFER_CHUNK, amountToRead);
            continue;
          }
        }
      }
    }

    KJ_IF_SOME(length, maybeLength) {
      if (runningTotal > length) {
        // Realistically runningTotal should never be more than length so we'll emit
        // a warning if it is just so we know. It would be indicative of a bug somewhere
        // in the implementation.
        KJ_LOG(WARNING, "ReadableStream provided more data than advertised", runningTotal, length);
      }
    }

    // Strip UTF-8 BOM if requested
    size_t skipBytes = 0;
    if ((option & ReadAllTextOption::STRIP_BOM) && parts.size() > 0 &&
        hasUtf8Bom(parts[0].asBytes())) {
      skipBytes = UTF8_BOM_SIZE;
      runningTotal -= UTF8_BOM_SIZE;
    }

    if (option & ReadAllTextOption::NULL_TERMINATE) {
      auto out = kj::heapArray<T>(runningTotal + 1);
      out[runningTotal] = '\0';
      copyInto<T>(out, parts.asPtr(), skipBytes);
      co_return kj::mv(out);
    }

    // As an optimization, if there's only a single part in the list, we can avoid
    // further copies.
    if (parts.size() == 1) {
      co_return kj::mv(parts[0]);
    }

    auto out = kj::heapArray<T>(runningTotal);
    copyInto<T>(out, parts.asPtr());
    co_return kj::mv(out);
  }

  template <typename T>
  void copyInto(kj::ArrayPtr<T> out, kj::ArrayPtr<kj::Array<T>> in, size_t skipBytes = 0) {
    for (auto& part: in) {
      if (out.size() == 0) {
        break;
      }
      // The skipBytes are used to skip the BOM on the first part only.
      KJ_DASSERT(skipBytes <= part.size());
      auto slicedPart = skipBytes ? part.slice(skipBytes) : part;
      skipBytes = 0;
      if (slicedPart.size() == 0) {
        continue;
      }
      KJ_DASSERT(slicedPart.size() <= out.size());
      out.first(slicedPart.size()).copyFrom(slicedPart);
      out = out.slice(slicedPart.size());
    }
  }
};

kj::Exception reasonToException(jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> maybeReason,
    kj::String defaultDescription = kj::str(JSG_EXCEPTION(Error) ": Stream was cancelled.")) {
  KJ_IF_SOME(reason, maybeReason) {
    return js.exceptionToKj(js.v8Ref(reason));
  } else {
    // We get here if the caller is something like `r.cancel()` (or `r.cancel(undefined)`).
    return kj::Exception(
        kj::Exception::Type::FAILED, __FILE__, __LINE__, kj::mv(defaultDescription));
  }
}

// =======================================================================================

// Adapt ReadableStreamSource to kj::AsyncInputStream's interface for use with `kj::newTee()`.
class TeeAdapter final: public kj::AsyncInputStream {
 public:
  explicit TeeAdapter(kj::Own<ReadableStreamSource> inner): inner(kj::mv(inner)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return inner->tryRead(buffer, minBytes, maxBytes);
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return inner->tryGetLength(StreamEncoding::IDENTITY);
  }

 private:
  kj::Own<ReadableStreamSource> inner;
};

class TeeBranch final: public ReadableStreamSource {
 public:
  explicit TeeBranch(kj::Own<kj::AsyncInputStream> inner): inner(kj::mv(inner)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return inner->tryRead(buffer, minBytes, maxBytes);
  }

  kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end) override {
#ifdef KJ_NO_RTTI
    // Yes, I'm paranoid.
    static_assert(!KJ_NO_RTTI, "Need RTTI for correctness");
#endif

    // HACK: If `output` is another TransformStream, we don't allow pumping to it, in order to
    //   guarantee that we can't create cycles. Note that currently TeeBranch only ever wraps
    //   TransformStreams, never system streams.
    JSG_REQUIRE(!isIdentityTransformStream(output), TypeError,
        "Inter-TransformStream ReadableStream.pipeTo() is not implemented.");

    // It is important we actually call `inner->pumpTo()` so that `kj::newTee()` is aware of this
    // pump operation's backpressure. So we can't use the default `ReadableStreamSource::pumpTo()`
    // implementation, and have to implement our own.

    PumpAdapter outputAdapter(output);
    co_await inner->pumpTo(outputAdapter);

    if (end) {
      co_await output.end();
    }

    // We only use `TeeBranch` when a locally-sourced stream was tee'd (because system streams
    // implement `tryTee()` in a different way that doesn't use `TeeBranch`). So, we know that
    // none of the pump can be performed without the IoContext active, and thus we do not
    // `KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING`.
    co_return;
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    if (encoding == StreamEncoding::IDENTITY) {
      return inner->tryGetLength();
    } else {
      return kj::none;
    }
  }

  kj::Maybe<Tee> tryTee(uint64_t limit) override {
    KJ_IF_SOME(t, inner->tryTee(limit)) {
      auto branch = kj::heap<TeeBranch>(newTeeErrorAdapter(kj::mv(t)));
      auto consumed = kj::heap<TeeBranch>(kj::mv(inner));
      return Tee{kj::mv(branch), kj::mv(consumed)};
    }

    return kj::none;
  }

  void cancel(kj::Exception reason) override {
    // TODO(someday): What to do?
  }

 private:
  // Adapt WritableStreamSink to kj::AsyncOutputStream's interface for use in
  // `TeeBranch::pumpTo()`. If you squint, the write logic looks very similar to TeeAdapter's
  // read logic.
  class PumpAdapter final: public kj::AsyncOutputStream {
   public:
    explicit PumpAdapter(WritableStreamSink& inner): inner(inner) {}

    kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
      return inner.write(buffer);
    }

    kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
      return inner.write(pieces);
    }

    kj::Promise<void> whenWriteDisconnected() override {
      KJ_UNIMPLEMENTED("whenWriteDisconnected() not expected on PumpAdapter");
    }

    WritableStreamSink& inner;
  };

  kj::Own<kj::AsyncInputStream> inner;
};
}  // namespace

// =======================================================================================

kj::Promise<DeferredProxy<void>> ReadableStreamSource::pumpTo(
    WritableStreamSink& output, bool end) {
  KJ_IF_SOME(p, output.tryPumpFrom(*this, end)) {
    return kj::mv(p);
  }

  // Non-optimized pumpTo() is presumed to require the IoContext to remain live, so don't do
  // anything in the deferred proxy part.
  return addNoopDeferredProxy(api::pumpTo(*this, output, end));
}

kj::Maybe<uint64_t> ReadableStreamSource::tryGetLength(StreamEncoding encoding) {
  return kj::none;
}

kj::Promise<kj::Array<byte>> ReadableStreamSource::readAllBytes(uint64_t limit) {
  try {
    AllReader allReader(*this, limit);
    co_return co_await allReader.readAllBytes();
  } catch (...) {
    // TODO(soon): Temporary logging.
    auto ex = kj::getCaughtExceptionAsKj();
    if (ex.getDescription().endsWith("exceeded before EOF.")) {
      LOG_WARNING_PERIODICALLY("NOSENTRY Internal Stream readAllBytes - Exceeded limit");
    }
    kj::throwFatalException(kj::mv(ex));
  }
}

kj::Promise<kj::String> ReadableStreamSource::readAllText(
    uint64_t limit, ReadAllTextOption option) {
  try {
    AllReader allReader(*this, limit);
    co_return co_await allReader.readAllText(option);
  } catch (...) {
    // TODO(soon): Temporary logging.
    auto ex = kj::getCaughtExceptionAsKj();
    if (ex.getDescription().endsWith("exceeded before EOF.")) {
      LOG_WARNING_PERIODICALLY("NOSENTRY Internal Stream readAllText - Exceeded limit");
    }
    kj::throwFatalException(kj::mv(ex));
  }
}

void ReadableStreamSource::cancel(kj::Exception reason) {}

kj::Maybe<ReadableStreamSource::Tee> ReadableStreamSource::tryTee(uint64_t limit) {
  return kj::none;
}

kj::Maybe<kj::Promise<DeferredProxy<void>>> WritableStreamSink::tryPumpFrom(
    ReadableStreamSource& input, bool end) {
  return kj::none;
}

// =======================================================================================

ReadableStreamInternalController::~ReadableStreamInternalController() noexcept(false) {
  if (readState.is<ReaderLocked>()) {
    readState.transitionTo<Unlocked>();
  }
}

jsg::Ref<ReadableStream> ReadableStreamInternalController::addRef() {
  return KJ_ASSERT_NONNULL(owner).addRef();
}

kj::Maybe<jsg::Promise<ReadResult>> ReadableStreamInternalController::read(
    jsg::Lock& js, kj::Maybe<ByobOptions> maybeByobOptions) {

  if (isPendingClosure) {
    return js.rejectedPromise<ReadResult>(
        js.v8TypeError("This ReadableStream belongs to an object that is closing."_kj));
  }

  v8::Local<v8::ArrayBuffer> store;
  size_t byteLength = 0;
  size_t byteOffset = 0;
  size_t atLeast = 1;

  KJ_IF_SOME(byobOptions, maybeByobOptions) {
    store = byobOptions.bufferView.getHandle(js)->Buffer();
    byteOffset = byobOptions.byteOffset;
    byteLength = byobOptions.byteLength;
    atLeast = byobOptions.atLeast.orDefault(atLeast);
    if (byobOptions.detachBuffer) {
      if (!store->IsDetachable()) {
        return js.rejectedPromise<ReadResult>(
            js.v8TypeError("Unable to use non-detachable ArrayBuffer"_kj));
      }
      auto backing = store->GetBackingStore();
      jsg::check(store->Detach(v8::Local<v8::Value>()));
      store = v8::ArrayBuffer::New(js.v8Isolate, kj::mv(backing));
    }
  }

  auto getOrInitStore = [&](bool errorCase = false) {
    if (store.IsEmpty()) {
      // In an error case, where store is not provided, we can use zero length
      byteLength = errorCase ? 0 : UnderlyingSource::DEFAULT_AUTO_ALLOCATE_CHUNK_SIZE;

      if (!v8::ArrayBuffer::MaybeNew(js.v8Isolate, byteLength).ToLocal(&store)) {
        return v8::Local<v8::ArrayBuffer>();
      }
    }
    return store;
  };

  disturbed = true;

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      if (maybeByobOptions != kj::none && FeatureFlags::get(js).getInternalStreamByobReturn()) {
        // When using the BYOB reader, we must return a sized-0 Uint8Array that is backed
        // by the ArrayBuffer passed in the options.
        auto theStore = getOrInitStore(true);
        if (theStore.IsEmpty()) {
          return js.rejectedPromise<ReadResult>(
              js.v8TypeError("Unable to allocate memory for read"_kj));
        }
        return js.resolvedPromise(ReadResult{
          .value = js.v8Ref(v8::Uint8Array::New(theStore, 0, 0).As<v8::Value>()),
          .done = true,
        });
      }
      return js.resolvedPromise(ReadResult{.done = true});
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<ReadResult>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(readable, Readable) {
      // TODO(conform): Requiring serialized read requests is non-conformant, but we've never had a
      //   use case for them. At one time, our implementation of TransformStream supported multiple
      //   simultaneous read requests, but it is highly unlikely that anyone relied on this. Our
      //   ReadableStream implementation that wraps native streams has never supported them, our
      //   TransformStream implementation is primarily (only?) used for constructing manually
      //   streamed Responses, and no teed ReadableStream has ever supported them.
      if (readPending) {
        return js.rejectedPromise<ReadResult>(js.v8TypeError(
            "This ReadableStream only supports a single pending read request at a time."_kj));
      }
      readPending = true;

      auto theStore = getOrInitStore();
      if (theStore.IsEmpty()) {
        return js.rejectedPromise<ReadResult>(
            js.v8TypeError("Unable to allocate memory for read"_kj));
      }

      // In the case the ArrayBuffer is detached/transfered while the read is pending, we
      // need to make sure that the ptr remains stable, so we grab a shared ptr to the
      // backing store and use that to get the pointer to the data. If the buffer is detached
      // while the read is pending, this does mean that the read data will end up being lost,
      // but there's not really a better option. The best we can do here is warn the user
      // that this is happening so they can avoid doing it in the future.
      // Also, the user really shouldn't do this because the read will end up completing into
      // the detached backing store still which could cause issues with whatever code now actually
      // owns the transfered buffer. Below we'll warn the user about this if it happens so they
      // can avoid doing it in the future.
      auto backing = theStore->GetBackingStore();

      auto ptr = static_cast<kj::byte*>(backing->Data());
      auto bytes = kj::arrayPtr(ptr + byteOffset, byteLength);

      auto promise = kj::evalNow([&] {
        return readable->tryRead(bytes.begin(), atLeast, bytes.size()).attach(kj::mv(backing));
      });
      KJ_IF_SOME(readerLock, readState.tryGetUnsafe<ReaderLocked>()) {
        promise = KJ_ASSERT_NONNULL(readerLock.getCanceler())->wrap(kj::mv(promise));
      }

      // TODO(soon): We use awaitIoLegacy() here because if the stream terminates in JavaScript in
      // this same isolate, then the promise may actually be waiting on JavaScript to do something,
      // and so should not be considered waiting on external I/O. We will need to use
      // registerPendingEvent() manually when reading from an external stream. Ideally, we would
      // refactor the implementation so that when waiting on a JavaScript stream, we strictly use
      // jsg::Promises and not kj::Promises, so that it doesn't look like I/O at all, and there's
      // no need to drop the isolate lock and take it again every time some data is read/written.
      // That's a larger refactor, though.
      auto& ioContext = IoContext::current();
      return ioContext.awaitIoLegacy(js, kj::mv(promise))
          .then(js,
              ioContext.addFunctor([this, store = js.v8Ref(store), byteOffset, byteLength,
                                       isByob = maybeByobOptions != kj::none](jsg::Lock& js,
                                       size_t amount) mutable -> jsg::Promise<ReadResult> {
        readPending = false;
        KJ_ASSERT(amount <= byteLength);
        if (amount == 0) {
          if (!state.is<StreamStates::Errored>()) {
            doClose(js);
          }
          KJ_IF_SOME(o, owner) {
            o.signalEof(js);
          }
          if (isByob && FeatureFlags::get(js).getInternalStreamByobReturn()) {
            // When using the BYOB reader, we must return a sized-0 Uint8Array that is backed
            // by the ArrayBuffer passed in the options.
            auto u8 = v8::Uint8Array::New(store.getHandle(js), 0, 0);
            return js.resolvedPromise(ReadResult{
              .value = js.v8Ref(u8.As<v8::Value>()),
              .done = true,
            });
          }
          return js.resolvedPromise(ReadResult{.done = true});
        }
        // Return a slice so the script can see how many bytes were read.

        // We have to check to see if the store was detached or resized while we were waiting
        // for the read to complete.
        auto handle = store.getHandle(js);
        if (handle->WasDetached()) {
          // If the buffer was detached, we resolve with a new zero-length ArrayBuffer.
          // The bytes that were read are lost, but this is a valid result.

          // Silly user, trix are for kids.
          IoContext::current().logWarningOnce(
              "A buffer that was being used for a read operation on a ReadableStream was detached "
              "while the read was pending. The read completed with a zero-length buffer and the data "
              "that was read is lost. Avoid detaching buffers that are being used for active read "
              "operations on streams, or use the streams_byob_reader_detaches_buffer compatibility "
              "flag, to prevent this from happening."_kj);

          auto buffer = v8::ArrayBuffer::New(js.v8Isolate, 0);
          return js.resolvedPromise(ReadResult{
            .value = js.v8Ref(v8::Uint8Array::New(buffer, 0, 0).As<v8::Value>()),
            .done = false,
          });
        }

        if (byteOffset + amount > handle->ByteLength()) {
          // If the buffer was resized smaller, we return a truncated result.
          // Any bytes that would have been written past the new end are lost.

          IoContext::current().logWarningOnce(
              "A buffer that was being used for a read operation on a ReadableStream was resized "
              "smaller while the read was pending. The read completed with a truncated buffer "
              "containing only the bytes that fit within the new size. Avoid resizing buffers that "
              "are being used for active read operations on streams, or use the "
              "streams_byob_reader_detaches_buffer compatibility flag, to prevent this from "
              "happening."_kj);

          amount = handle->ByteLength() > byteOffset ? handle->ByteLength() - byteOffset : 0;
        }

        return js.resolvedPromise(ReadResult{
          .value = js.v8Ref(
              v8::Uint8Array::New(store.getHandle(js), byteOffset, amount).As<v8::Value>()),
          .done = false});
      }),
              ioContext.addFunctor(
                  [this](jsg::Lock& js, jsg::Value reason) -> jsg::Promise<ReadResult> {
        readPending = false;
        if (!state.is<StreamStates::Errored>()) {
          doError(js, reason.getHandle(js));
        }
        return js.rejectedPromise<ReadResult>(kj::mv(reason));
      }));
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<jsg::Promise<DrainingReadResult>> ReadableStreamInternalController::drainingRead(
    jsg::Lock& js, size_t maxRead) {
  // InternalController does not support draining reads fully since all reads are
  // async. We implement a simplified version that just performs a normal read
  // like read(). The significant difference is that with JS-backed streams, a draining
  // read will pull any already enqueued data from the stream buffer and try synchronously
  // pumping the stream for more data until either maxRead is satisfied or the stream
  // indicates EOF, error, or that it needs to wait for more data. Internal streams have
  // no such internal buffering and never provide data synchronously so drainingRead
  // is effectively the same as read().

  if (isPendingClosure) {
    return js.rejectedPromise<DrainingReadResult>(
        js.v8TypeError("This ReadableStream belongs to an object that is closing."_kj));
  }

  static constexpr size_t kAtLeast = 1;

  disturbed = true;

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return js.resolvedPromise(DrainingReadResult{.done = true});
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<DrainingReadResult>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(readable, Readable) {
      if (readPending) {
        return js.rejectedPromise<DrainingReadResult>(js.v8TypeError(
            "This ReadableStream only supports a single pending read request at a time."_kj));
      }
      readPending = true;

      // TODO(later): In the case that maxRead is large, we may consider splitting this into
      // multiple reads to avoid allocating too large of a buffer at once. The draining read
      // result can handle multiple chunks so this would be feasible at the cost of more
      // read calls. For now we just do a single read up to maxRead.
      // At the very least, we cap maxRead to some reasonable limit to avoid
      // potential OOM issues.
      static constexpr size_t kMaxReadCap = 1 * 1024 * 1024;  // 1 MB
      maxRead = kj::min(maxRead, kMaxReadCap);

      if (maxRead == 0) {
        // No data requested, return empty result.
        // This really shouldn't ever happen but let's handle it gracefully.
        readPending = false;
        return js.resolvedPromise(DrainingReadResult{
          .chunks = nullptr,
          .done = false,
        });
      }

      auto store = kj::heapArray<kj::byte>(maxRead);

      auto promise =
          kj::evalNow([&] { return readable->tryRead(store.begin(), kAtLeast, store.size()); });
      KJ_IF_SOME(readerLock, readState.tryGetUnsafe<ReaderLocked>()) {
        promise = KJ_ASSERT_NONNULL(readerLock.getCanceler())->wrap(kj::mv(promise));
      }

      auto& ioContext = IoContext::current();
      return ioContext.awaitIoLegacy(js, kj::mv(promise))
          .then(js,
              ioContext.addFunctor([this, store = kj::mv(store)](jsg::Lock& js,
                                       size_t amount) mutable -> jsg::Promise<DrainingReadResult> {
        readPending = false;
        KJ_ASSERT(amount <= store.size());
        if (amount == 0) {
          if (!state.is<StreamStates::Errored>()) {
            doClose(js);
          }
          KJ_IF_SOME(o, owner) {
            o.signalEof(js);
          }
          return js.resolvedPromise(DrainingReadResult{.done = true});
        }
        // Return a slice so the script can see how many bytes were read.
        return js.resolvedPromise(DrainingReadResult{
          .chunks = kj::arr(store.slice(0, amount).attach(kj::mv(store))), .done = false});
      }),
              ioContext.addFunctor(
                  [this](jsg::Lock& js, jsg::Value reason) -> jsg::Promise<DrainingReadResult> {
        readPending = false;
        if (!state.is<StreamStates::Errored>()) {
          doError(js, reason.getHandle(js));
        }
        return js.rejectedPromise<DrainingReadResult>(kj::mv(reason));
      }));
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<void> ReadableStreamInternalController::pipeTo(
    jsg::Lock& js, WritableStreamController& destination, PipeToOptions options) {

  KJ_DASSERT(!isLockedToReader());
  KJ_DASSERT(!destination.isLockedToWriter());

  if (isPendingClosure) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This ReadableStream belongs to an object that is closing."_kj));
  }

  disturbed = true;
  KJ_IF_SOME(promise,
      destination.tryPipeFrom(js, KJ_ASSERT_NONNULL(owner).addRef(), kj::mv(options))) {
    return kj::mv(promise);
  }

  return js.rejectedPromise<void>(
      js.v8TypeError("This ReadableStream cannot be piped to this WritableStream."_kj));
}

jsg::Promise<void> ReadableStreamInternalController::cancel(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  disturbed = true;

  KJ_IF_SOME(errored, state.tryGetUnsafe<StreamStates::Errored>()) {
    return js.rejectedPromise<void>(errored.getHandle(js));
  }

  doCancel(js, maybeReason);

  return js.resolvedPromise();
}

void ReadableStreamInternalController::doCancel(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  auto exception = reasonToException(js, maybeReason);
  KJ_IF_SOME(locked, readState.tryGetUnsafe<ReaderLocked>()) {
    KJ_IF_SOME(canceler, locked.getCanceler()) {
      canceler->cancel(kj::cp(exception));
    }
  }
  KJ_IF_SOME(readable, state.tryGetUnsafe<Readable>()) {
    readable->cancel(kj::mv(exception));
    doClose(js);
  }
}

void ReadableStreamInternalController::doClose(jsg::Lock& js) {
  // If already in a terminal state, nothing to do.
  if (state.isTerminal()) return;

  state.transitionTo<StreamStates::Closed>();
  KJ_IF_SOME(locked, readState.tryGetUnsafe<ReaderLocked>()) {
    maybeResolvePromise(js, locked.getClosedFulfiller());
  } else {
    (void)readState.transitionFromTo<PipeLocked, Unlocked>();
  }
}

void ReadableStreamInternalController::doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
  // If already in a terminal state, nothing to do.
  if (state.isTerminal()) return;

  state.transitionTo<StreamStates::Errored>(js.v8Ref(reason));
  KJ_IF_SOME(locked, readState.tryGetUnsafe<ReaderLocked>()) {
    maybeRejectPromise<void>(js, locked.getClosedFulfiller(), reason);
  } else {
    (void)readState.transitionFromTo<PipeLocked, Unlocked>();
  }
}

ReadableStreamController::Tee ReadableStreamInternalController::tee(jsg::Lock& js) {
  JSG_REQUIRE(
      !isLockedToReader(), TypeError, "This ReadableStream is currently locked to a reader.");
  JSG_REQUIRE(
      !isPendingClosure, TypeError, "This ReadableStream belongs to an object that is closing.");
  readState.transitionTo<Locked>();
  disturbed = true;
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      // Create two closed ReadableStreams.
      return Tee{
        .branch1 = js.alloc<ReadableStream>(kj::heap<ReadableStreamInternalController>(closed)),
        .branch2 = js.alloc<ReadableStream>(kj::heap<ReadableStreamInternalController>(closed)),
      };
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      // Create two errored ReadableStreams.
      return Tee{
        .branch1 = js.alloc<ReadableStream>(
            kj::heap<ReadableStreamInternalController>(errored.addRef(js))),
        .branch2 = js.alloc<ReadableStream>(
            kj::heap<ReadableStreamInternalController>(errored.addRef(js))),
      };
    }
    KJ_CASE_ONEOF(readable, Readable) {
      auto& ioContext = IoContext::current();

      auto makeTee = [&](kj::Own<ReadableStreamSource> b1,
                         kj::Own<ReadableStreamSource> b2) -> Tee {
        doClose(js);
        return Tee{
          .branch1 = js.alloc<ReadableStream>(ioContext, kj::mv(b1)),
          .branch2 = js.alloc<ReadableStream>(ioContext, kj::mv(b2)),
        };
      };

      auto bufferLimit = ioContext.getLimitEnforcer().getBufferingLimit();
      KJ_IF_SOME(tee, readable->tryTee(bufferLimit)) {
        // This ReadableStreamSource has an optimized tee implementation.
        return makeTee(kj::mv(tee.branches[0]), kj::mv(tee.branches[1]));
      }

      auto tee = kj::newTee(kj::heap<TeeAdapter>(kj::mv(readable)), bufferLimit);

      return makeTee(kj::heap<TeeBranch>(newTeeErrorAdapter(kj::mv(tee.branches[0]))),
          kj::heap<TeeBranch>(newTeeErrorAdapter(kj::mv(tee.branches[1]))));
    }
  }

  KJ_UNREACHABLE;
}

kj::Maybe<kj::Own<ReadableStreamSource>> ReadableStreamInternalController::removeSource(
    jsg::Lock& js, bool ignoreDisturbed) {
  JSG_REQUIRE(
      !isLockedToReader(), TypeError, "This ReadableStream is currently locked to a reader.");
  JSG_REQUIRE(!disturbed || ignoreDisturbed, TypeError, "This ReadableStream is disturbed.");

  readState.transitionTo<Locked>();
  disturbed = true;

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      class NullSource final: public ReadableStreamSource {
       public:
        kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
          return static_cast<size_t>(0);
        }

        kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
          return static_cast<uint64_t>(0);
        }
      };

      return kj::heap<NullSource>();
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      kj::throwFatalException(js.exceptionToKj(errored.addRef(js)));
    }
    KJ_CASE_ONEOF(readable, Readable) {
      auto result = kj::mv(readable);
      state.transitionTo<StreamStates::Closed>();
      return kj::Maybe<kj::Own<ReadableStreamSource>>(kj::mv(result));
    }
  }

  KJ_UNREACHABLE;
}

bool ReadableStreamInternalController::lockReader(jsg::Lock& js, Reader& reader) {
  if (isLockedToReader()) {
    return false;
  }

  auto prp = js.newPromiseAndResolver<void>();
  prp.promise.markAsHandled(js);

  auto lock = ReaderLocked(
      reader, kj::mv(prp.resolver), IoContext::current().addObject(kj::heap<kj::Canceler>()));

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      maybeResolvePromise(js, lock.getClosedFulfiller());
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      maybeRejectPromise<void>(js, lock.getClosedFulfiller(), errored.getHandle(js));
    }
    KJ_CASE_ONEOF(readable, Readable) {
      // Nothing to do.
    }
  }

  readState.transitionTo<ReaderLocked>(kj::mv(lock));
  reader.attach(*this, kj::mv(prp.promise));
  return true;
}

void ReadableStreamInternalController::releaseReader(
    Reader& reader, kj::Maybe<jsg::Lock&> maybeJs) {
  KJ_IF_SOME(locked, readState.tryGetUnsafe<ReaderLocked>()) {
    KJ_ASSERT(&locked.getReader() == &reader);
    KJ_IF_SOME(js, maybeJs) {
      KJ_IF_SOME(canceler, locked.getCanceler()) {
        JSG_REQUIRE(canceler->isEmpty(), TypeError,
            "Cannot call releaseLock() on a reader with outstanding read promises.");
      }
      maybeRejectPromise<void>(js, locked.getClosedFulfiller(),
          js.v8TypeError("This ReadableStream reader has been released."_kj));
    }
    locked.clear();

    // When maybeJs is nullptr, that means releaseReader was called when the reader is
    // being deconstructed and not as the result of explicitly calling releaseLock. In
    // that case, we don't want to change the lock state itself because we do not have
    // an isolate lock. Clearing the lock above will free the lock state while keeping the
    // ReadableStream marked as locked.
    if (maybeJs != kj::none) {
      readState.transitionTo<Unlocked>();
    }
  }
}

void WritableStreamInternalController::Writable::abort(kj::Exception&& ex) {
  canceler.cancel(kj::cp(ex));
  sink->abort(kj::mv(ex));
}

WritableStreamInternalController::~WritableStreamInternalController() noexcept(false) {
  if (writeState.is<WriterLocked>()) {
    writeState.transitionTo<Unlocked>();
  }
}

jsg::Ref<WritableStream> WritableStreamInternalController::addRef() {
  return KJ_ASSERT_NONNULL(owner).addRef();
}

jsg::Promise<void> WritableStreamInternalController::write(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> value) {
  if (isPendingClosure) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This WritableStream belongs to an object that is closing."_kj));
  }
  if (isClosedOrClosing()) {
    return js.rejectedPromise<void>(js.v8TypeError("This WritableStream has been closed."_kj));
  }
  if (isPiping()) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This WritableStream is currently being piped to."_kj));
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      // Handled by isClosedOrClosing().
      KJ_UNREACHABLE;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<void>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(writable, IoOwn<Writable>) {
      if (value == kj::none) {
        return js.resolvedPromise();
      }
      auto chunk = KJ_ASSERT_NONNULL(value);

      std::shared_ptr<v8::BackingStore> store;
      size_t byteLength = 0;
      size_t byteOffset = 0;
      if (chunk->IsArrayBuffer()) {
        auto buffer = chunk.As<v8::ArrayBuffer>();
        store = buffer->GetBackingStore();
        byteLength = buffer->ByteLength();
      } else if (chunk->IsArrayBufferView()) {
        auto view = chunk.As<v8::ArrayBufferView>();
        store = view->Buffer()->GetBackingStore();
        byteLength = view->ByteLength();
        byteOffset = view->ByteOffset();
      } else if (chunk->IsString()) {
        // TODO(later): This really ought to return a rejected promise and not a sync throw.
        // This case caused me a moment of confusion during testing, so I think it's worth
        // a specific error message.
        throwTypeErrorAndConsoleWarn(
            "This TransformStream is being used as a byte stream, but received a string on its "
            "writable side. If you wish to write a string, you'll probably want to explicitly "
            "UTF-8-encode it with TextEncoder.");
      } else {
        // TODO(later): This really ought to return a rejected promise and not a sync throw.
        throwTypeErrorAndConsoleWarn(
            "This TransformStream is being used as a byte stream, but received an object of "
            "non-ArrayBuffer/ArrayBufferView type on its writable side.");
      }

      if (byteLength == 0) {
        return js.resolvedPromise();
      }

      auto prp = js.newPromiseAndResolver<void>();
      adjustWriteBufferSize(js, byteLength);
      KJ_IF_SOME(o, observer) {
        o->onChunkEnqueued(byteLength);
      }
      auto ptr =
          kj::ArrayPtr<kj::byte>(static_cast<kj::byte*>(store->Data()) + byteOffset, byteLength);
      if (store->IsShared()) {
        throwTypeErrorAndConsoleWarn(
            "Cannot construct an array buffer from a shared backing store");
      }
      queue.push_back(
          WriteEvent{.outputLock = IoContext::current().waitForOutputLocksIfNecessaryIoOwn(),
            .event = kj::heap<Write>({
              .promise = kj::mv(prp.resolver),
              .totalBytes = store->ByteLength(),
              .ownBytes = js.v8Ref(v8::ArrayBuffer::New(js.v8Isolate, kj::mv(store))),
              .bytes = ptr,
            })});

      ensureWriting(js);
      return kj::mv(prp.promise);
    }
  }

  KJ_UNREACHABLE;
}

void WritableStreamInternalController::adjustWriteBufferSize(jsg::Lock& js, int64_t amount) {
  KJ_DASSERT(amount >= 0 || std::abs(amount) <= currentWriteBufferSize);
  currentWriteBufferSize += amount;
  KJ_IF_SOME(highWaterMark, maybeHighWaterMark) {
    int64_t desiredSize = highWaterMark - currentWriteBufferSize;
    updateBackpressure(js, desiredSize <= 0);
  }
}

void WritableStreamInternalController::updateBackpressure(jsg::Lock& js, bool backpressure) {
  KJ_IF_SOME(writerLock, writeState.tryGetUnsafe<WriterLocked>()) {
    if (backpressure) {
      // Per the spec, when backpressure is updated and is true, we replace the existing
      // ready promise on the writer with a new pending promise, regardless of whether
      // the existing one is resolved or not.
      auto prp = js.newPromiseAndResolver<void>();
      prp.promise.markAsHandled(js);
      writerLock.setReadyFulfiller(js, prp);
      return;
    }

    // When backpressure is updated and is false, we resolve the ready promise on the writer
    maybeResolvePromise(js, writerLock.getReadyFulfiller());
  }
}

void WritableStreamInternalController::setHighWaterMark(uint64_t highWaterMark) {
  maybeHighWaterMark = highWaterMark;
}

jsg::Promise<void> WritableStreamInternalController::closeImpl(jsg::Lock& js, bool markAsHandled) {
  if (isClosedOrClosing()) {
    return js.resolvedPromise();
  }
  if (isPiping()) {
    auto reason = js.v8TypeError("This WritableStream is currently being piped to."_kj);
    return rejectedMaybeHandledPromise<void>(js, reason, markAsHandled);
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      // Handled by isClosedOrClosing().
      KJ_UNREACHABLE;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      auto reason = errored.getHandle(js);
      return rejectedMaybeHandledPromise<void>(js, reason, markAsHandled);
    }
    KJ_CASE_ONEOF(writable, IoOwn<Writable>) {
      auto prp = js.newPromiseAndResolver<void>();
      if (markAsHandled) {
        prp.promise.markAsHandled(js);
      }
      queue.push_back(
          WriteEvent{.outputLock = IoContext::current().waitForOutputLocksIfNecessaryIoOwn(),
            .event = kj::heap<Close>({.promise = kj::mv(prp.resolver)})});
      ensureWriting(js);
      return kj::mv(prp.promise);
    }
  }

  KJ_UNREACHABLE;
}

jsg::Promise<void> WritableStreamInternalController::close(jsg::Lock& js, bool markAsHandled) {
  KJ_IF_SOME(closureWaitable, maybeClosureWaitable) {
    // If we're already waiting on the closure waitable, then we do not want to try scheduling
    // it again, let's just wait for the existing one to be resolved.
    if (waitingOnClosureWritableAlready) {
      return closureWaitable.whenResolved(js);
    }
    waitingOnClosureWritableAlready = true;
    auto promise = closureWaitable.then(js, [markAsHandled, this](jsg::Lock& js) {
      return closeImpl(js, markAsHandled);
    }, [](jsg::Lock& js, jsg::Value) {
      // Ignore rejection as it will be reported in the Socket's `closed`/`opened` promises
      // instead.
      return js.resolvedPromise();
    });
    maybeClosureWaitable = promise.whenResolved(js);
    return kj::mv(promise);
  } else {
    return closeImpl(js, markAsHandled);
  }
}

jsg::Promise<void> WritableStreamInternalController::flush(jsg::Lock& js, bool markAsHandled) {
  if (isClosedOrClosing()) {
    auto reason = js.v8TypeError("This WritableStream has been closed."_kj);
    return rejectedMaybeHandledPromise<void>(js, reason, markAsHandled);
  }
  if (isPiping()) {
    auto reason = js.v8TypeError("This WritableStream is currently being piped to."_kj);
    return rejectedMaybeHandledPromise<void>(js, reason, markAsHandled);
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      // Handled by isClosedOrClosing().
      KJ_UNREACHABLE;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      auto reason = errored.getHandle(js);
      return rejectedMaybeHandledPromise<void>(js, reason, markAsHandled);
    }
    KJ_CASE_ONEOF(writable, IoOwn<Writable>) {
      auto prp = js.newPromiseAndResolver<void>();
      if (markAsHandled) {
        prp.promise.markAsHandled(js);
      }
      queue.push_back(
          WriteEvent{.outputLock = IoContext::current().waitForOutputLocksIfNecessaryIoOwn(),
            .event = kj::heap<Flush>({.promise = kj::mv(prp.resolver)})});
      ensureWriting(js);
      return kj::mv(prp.promise);
    }
  }

  KJ_UNREACHABLE;
}

jsg::Promise<void> WritableStreamInternalController::abort(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  // While it may be confusing to users to throw `undefined` rather than a more helpful Error here,
  // doing so is required by the relevant spec:
  // https://streams.spec.whatwg.org/#writable-stream-abort
  return doAbort(js, maybeReason.orDefault(js.v8Undefined()));
}

jsg::Promise<void> WritableStreamInternalController::doAbort(
    jsg::Lock& js, v8::Local<v8::Value> reason, AbortOptions options) {
  // If maybePendingAbort is set, then the returned abort promise will be rejected
  // with the specified error once the abort is completed, otherwise the promise will
  // be resolved with undefined.

  // If there is already an abort pending, return that pending promise
  // instead of trying to schedule another.
  KJ_IF_SOME(pendingAbort, maybePendingAbort) {
    pendingAbort->reject = options.reject;
    auto promise = pendingAbort->whenResolved(js);
    if (options.handled) {
      promise.markAsHandled(js);
    }
    return kj::mv(promise);
  }

  KJ_IF_SOME(writable, state.tryGetUnsafe<IoOwn<Writable>>()) {
    auto exception = js.exceptionToKj(js.v8Ref(reason));

    if (FeatureFlags::get(js).getInternalWritableStreamAbortClearsQueue()) {
      // If this flag is set, we will clear the queue proactively and immediately
      // error the stream rather than handling the abort lazily. In this case, the
      // stream will be put into an errored state immediately after draining the
      // queue. All pending writes and other operations in the queue will be rejected
      // immediately and an immediately resolved or rejected promise will be returned.
      writable->abort(kj::cp(exception));
      drain(js, reason);
      return options.reject ? rejectedMaybeHandledPromise<void>(js, reason, options.handled)
                            : js.resolvedPromise();
    }

    if (queue.empty()) {
      writable->abort(kj::cp(exception));
      doError(js, reason);
      return options.reject ? rejectedMaybeHandledPromise<void>(js, reason, options.handled)
                            : js.resolvedPromise();
    }

    maybePendingAbort = kj::heap<PendingAbort>(js, reason, options.reject);
    auto promise = KJ_ASSERT_NONNULL(maybePendingAbort)->whenResolved(js);
    if (options.handled) {
      promise.markAsHandled(js);
    }
    return kj::mv(promise);
  }

  return options.reject ? rejectedMaybeHandledPromise<void>(js, reason, options.handled)
                        : js.resolvedPromise();
}

kj::Maybe<jsg::Promise<void>> WritableStreamInternalController::tryPipeFrom(
    jsg::Lock& js, jsg::Ref<ReadableStream> source, PipeToOptions options) {

  // The ReadableStream source here can be either a JavaScript-backed ReadableStream
  // or ReadableStreamSource-backed.
  //
  // If the source is ReadableStreamSource-backed, then we can use kj's low level mechanisms
  // for piping the data. If the source is JavaScript-backed, then we need to rely on the
  // JavaScript-based Promise API for piping the data.

  auto preventAbort = options.preventAbort.orDefault(false);
  auto preventClose = options.preventClose.orDefault(false);
  auto preventCancel = options.preventCancel.orDefault(false);
  auto pipeThrough = options.pipeThrough;

  if (isPiping()) {
    auto reason = js.v8TypeError("This WritableStream is currently being piped to."_kj);
    return rejectedMaybeHandledPromise<void>(js, reason, pipeThrough);
  }

  // If a signal is provided, we need to check that it is not already triggered. If it
  // is, we return a rejected promise using the signal's reason.
  KJ_IF_SOME(signal, options.signal) {
    if (signal->getAborted(js)) {
      return rejectedMaybeHandledPromise<void>(js, signal->getReason(js), pipeThrough);
    }
  }

  // With either type of source, our first step is to acquire the source pipe lock. This
  // will help abstract most of the details of which type of source we're working with.
  auto& sourceLock = KJ_ASSERT_NONNULL(source->getController().tryPipeLock());

  // Let's also acquire the destination pipe lock.
  writeState.transitionTo<PipeLocked>(*source);

  // If the source has errored, the spec requires us to reject the pipe promise and, if preventAbort
  // is false, error the destination (Propagate error forward). The errored source will be unlocked
  // immediately. The destination will be unlocked once the abort completes.
  KJ_IF_SOME(errored, sourceLock.tryGetErrored(js)) {
    sourceLock.release(js);
    if (!preventAbort) {
      if (state.tryGetUnsafe<IoOwn<Writable>>() != kj::none) {
        return doAbort(js, errored, {.reject = true, .handled = pipeThrough});
      }
    }

    // If preventAbort was true, we're going to unlock the destination now.
    writeState.transitionTo<Unlocked>();
    return rejectedMaybeHandledPromise<void>(js, errored, pipeThrough);
  }

  // If the destination has errored, the spec requires us to reject the pipe promise and, if
  // preventCancel is false, error the source (Propagate error backward). The errored destination
  // will be unlocked immediately.
  KJ_IF_SOME(errored, state.tryGetUnsafe<StreamStates::Errored>()) {
    writeState.transitionTo<Unlocked>();
    if (!preventCancel) {
      sourceLock.release(js, errored.getHandle(js));
    } else {
      sourceLock.release(js);
    }
    return rejectedMaybeHandledPromise<void>(js, errored.getHandle(js), pipeThrough);
  }

  // If the source has closed, the spec requires us to close the destination if preventClose
  // is false (Propagate closing forward). The source is unlocked immediately. The destination
  // will be unlocked as soon as the close completes.
  if (sourceLock.isClosed()) {
    sourceLock.release(js);
    if (!preventClose) {
      // The spec would have us check to see if `destination` is errored and, if so, return its
      // stored error. But if `destination` were errored, we would already have caught that case
      // above. The spec is probably concerned about cases where the readable and writable sides
      // transition to such states in a racey way. But our pump implementation will take care of
      // this naively.
      KJ_ASSERT(!state.is<StreamStates::Errored>());
      if (!isClosedOrClosing()) {
        return close(js);
      }
    }
    writeState.transitionTo<Unlocked>();
    return js.resolvedPromise();
  }

  // If the destination has closed, the spec requires us to close the source if
  // preventCancel is false (Propagate closing backward).
  if (isClosedOrClosing()) {
    auto destClosed = js.v8TypeError("This destination writable stream is closed."_kj);
    writeState.transitionTo<Unlocked>();

    if (!preventCancel) {
      sourceLock.release(js, destClosed);
    } else {
      sourceLock.release(js);
    }

    return rejectedMaybeHandledPromise<void>(js, destClosed, pipeThrough);
  }

  // The pipe will continue until either the source closes or errors, or until the destination
  // closes or errors. In either case, both will end up being closed or errored, which will
  // release the locks on both.
  //
  // For either type of source, our next step is to wait for the write loop to process the
  // pending Pipe event we queue below.
  auto prp = js.newPromiseAndResolver<void>();
  if (pipeThrough) {
    prp.promise.markAsHandled(js);
  }
  queue.push_back(WriteEvent{
    .outputLock = IoContext::current().waitForOutputLocksIfNecessaryIoOwn(),
    .event = kj::heap<Pipe>(*this, sourceLock, kj::mv(prp.resolver), preventAbort, preventClose,
        preventCancel, kj::mv(options.signal)),
  });
  ensureWriting(js);
  return kj::mv(prp.promise);
}

kj::Maybe<kj::Own<WritableStreamSink>> WritableStreamInternalController::removeSink(jsg::Lock& js) {
  JSG_REQUIRE(
      !isLockedToWriter(), TypeError, "This WritableStream is currently locked to a writer.");
  JSG_REQUIRE(!isClosedOrClosing(), TypeError, "This WritableStream is closed.");

  writeState.transitionTo<Locked>();

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      // Handled by the isClosedOrClosing() check above;
      KJ_UNREACHABLE;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      kj::throwFatalException(js.exceptionToKj(errored.addRef(js)));
    }
    KJ_CASE_ONEOF(writable, IoOwn<Writable>) {
      auto result = kj::mv(writable->sink);
      state.transitionTo<StreamStates::Closed>();
      return kj::Maybe<kj::Own<WritableStreamSink>>(kj::mv(result));
    }
  }

  KJ_UNREACHABLE;
}

void WritableStreamInternalController::detach(jsg::Lock& js) {
  JSG_REQUIRE(
      !isLockedToWriter(), TypeError, "This WritableStream is currently locked to a writer.");
  JSG_REQUIRE(!isClosedOrClosing(), TypeError, "This WritableStream is closed.");

  writeState.transitionTo<Locked>();

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      // Handled by the isClosedOrClosing() check above;
      KJ_UNREACHABLE;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      kj::throwFatalException(js.exceptionToKj(errored.addRef(js)));
    }
    KJ_CASE_ONEOF(writable, IoOwn<Writable>) {
      state.transitionTo<StreamStates::Closed>();
      return;
    }
  }

  KJ_UNREACHABLE;
}

kj::Maybe<int> WritableStreamInternalController::getDesiredSize() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return 0;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return kj::none;
    }
    KJ_CASE_ONEOF(writable, IoOwn<Writable>) {
      KJ_IF_SOME(highWaterMark, maybeHighWaterMark) {
        return highWaterMark - currentWriteBufferSize;
      }
      return 1;
    }
  }

  KJ_UNREACHABLE;
}

bool WritableStreamInternalController::lockWriter(jsg::Lock& js, Writer& writer) {
  if (isLockedToWriter()) {
    return false;
  }

  auto closedPrp = js.newPromiseAndResolver<void>();
  closedPrp.promise.markAsHandled(js);

  auto readyPrp = js.newPromiseAndResolver<void>();
  readyPrp.promise.markAsHandled(js);

  auto lock = WriterLocked(writer, kj::mv(closedPrp.resolver), kj::mv(readyPrp.resolver));

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      maybeResolvePromise(js, lock.getClosedFulfiller());
      maybeResolvePromise(js, lock.getReadyFulfiller());
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      maybeRejectPromise<void>(js, lock.getClosedFulfiller(), errored.getHandle(js));
      maybeRejectPromise<void>(js, lock.getReadyFulfiller(), errored.getHandle(js));
    }
    KJ_CASE_ONEOF(writable, IoOwn<Writable>) {
      maybeResolvePromise(js, lock.getReadyFulfiller());
    }
  }

  writeState.transitionTo<WriterLocked>(kj::mv(lock));
  writer.attach(js, *this, kj::mv(closedPrp.promise), kj::mv(readyPrp.promise));
  return true;
}

void WritableStreamInternalController::releaseWriter(
    Writer& writer, kj::Maybe<jsg::Lock&> maybeJs) {
  KJ_IF_SOME(locked, writeState.tryGetUnsafe<WriterLocked>()) {
    KJ_ASSERT(&locked.getWriter() == &writer);
    KJ_IF_SOME(js, maybeJs) {
      maybeRejectPromise<void>(js, locked.getClosedFulfiller(),
          js.v8TypeError("This WritableStream writer has been released."_kj));
    }
    locked.clear();

    // When maybeJs is nullptr, that means releaseWriter was called when the writer is
    // being deconstructed and not as the result of explicitly calling releaseLock and
    // we do not have an isolate lock. In that case, we don't want to change the lock
    // state itself. Clearing the lock above will free the lock state while keeping the
    // WritableStream marked as locked.
    if (maybeJs != kj::none) {
      writeState.transitionTo<Unlocked>();
    }
  }
}

bool WritableStreamInternalController::isClosedOrClosing() {

  bool isClosing = !queue.empty() && queue.back().event.is<kj::Own<Close>>();
  bool isFlushing = !queue.empty() && queue.back().event.is<kj::Own<Flush>>();
  return state.is<StreamStates::Closed>() || isClosing || isFlushing;
}

bool WritableStreamInternalController::isPiping() {
  return state.is<IoOwn<Writable>>() && !queue.empty() && queue.back().event.is<kj::Own<Pipe>>();
}

bool WritableStreamInternalController::isErrored() {
  return state.is<StreamStates::Errored>();
}

void WritableStreamInternalController::doClose(jsg::Lock& js) {
  // If already in a terminal state, nothing to do.
  if (state.isTerminal()) return;

  state.transitionTo<StreamStates::Closed>();
  KJ_IF_SOME(locked, writeState.tryGetUnsafe<WriterLocked>()) {
    maybeResolvePromise(js, locked.getClosedFulfiller());
    maybeResolvePromise(js, locked.getReadyFulfiller());
    writeState.transitionTo<Locked>();
  } else {
    (void)writeState.transitionFromTo<PipeLocked, Unlocked>();
  }
  PendingAbort::dequeue(maybePendingAbort);
}

void WritableStreamInternalController::doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
  // If already in a terminal state, nothing to do.
  if (state.isTerminal()) return;

  state.transitionTo<StreamStates::Errored>(js.v8Ref(reason));
  KJ_IF_SOME(locked, writeState.tryGetUnsafe<WriterLocked>()) {
    maybeRejectPromise<void>(js, locked.getClosedFulfiller(), reason);
    maybeResolvePromise(js, locked.getReadyFulfiller());
    writeState.transitionTo<Locked>();
  } else {
    (void)writeState.transitionFromTo<PipeLocked, Unlocked>();
  }
  PendingAbort::dequeue(maybePendingAbort);
}

void WritableStreamInternalController::ensureWriting(jsg::Lock& js) {
  auto& ioContext = IoContext::current();
  if (queue.size() == 1) {
    ioContext.addTask(ioContext.awaitJs(js, writeLoop(js, ioContext)).attach(addRef()));
  }
}

jsg::Promise<void> WritableStreamInternalController::writeLoop(
    jsg::Lock& js, IoContext& ioContext) {
  if (queue.empty()) {
    return js.resolvedPromise();
  } else KJ_IF_SOME(promise, queue.front().outputLock) {
    return ioContext.awaitIo(js, kj::mv(*promise),
        [this](jsg::Lock& js) -> jsg::Promise<void> { return writeLoopAfterFrontOutputLock(js); });
  } else {
    return writeLoopAfterFrontOutputLock(js);
  }
}

void WritableStreamInternalController::finishClose(jsg::Lock& js) {
  KJ_IF_SOME(pendingAbort, PendingAbort::dequeue(maybePendingAbort)) {
    pendingAbort->complete(js);
  }

  doClose(js);
}

void WritableStreamInternalController::finishError(jsg::Lock& js, v8::Local<v8::Value> reason) {
  KJ_IF_SOME(pendingAbort, PendingAbort::dequeue(maybePendingAbort)) {
    // In this case, and only this case, we ignore any pending rejection
    // that may be stored in the pendingAbort. The current exception takes
    // precedence.
    pendingAbort->fail(js, reason);
  }

  doError(js, reason);
}

jsg::Promise<void> WritableStreamInternalController::writeLoopAfterFrontOutputLock(jsg::Lock& js) {
  auto& ioContext = IoContext::current();

  // This helper function is just used to enhance the assert logging when checking
  // that the request in flight is the one we expect.
  static constexpr auto inspectQueue = [](auto& queue, kj::StringPtr name) {
    if (queue.size() > 1) {
      kj::Vector<kj::String> events;
      for (auto& event: queue) {
        KJ_SWITCH_ONEOF(event.event) {
          KJ_CASE_ONEOF(write, kj::Own<Write>) {
            events.add(kj::str("Write"));
          }
          KJ_CASE_ONEOF(flush, kj::Own<Flush>) {
            events.add(kj::str("Flush"));
          }
          KJ_CASE_ONEOF(close, kj::Own<Close>) {
            events.add(kj::str("Close"));
          }
          KJ_CASE_ONEOF(pipe, kj::Own<Pipe>) {
            events.add(kj::str("Pipe"));
          }
        }
      }
      return kj::str("Too many events in internal writablestream queue: ",
          kj::delimited(kj::mv(events), ", "));
    }
    return kj::String();
  };

  const auto makeChecker = [this]() {
    // Make a helper function that asserts that the queue did not change state during a write/close
    // operation. We normally only pop/drain the queue after write/close completion. We drain the
    // queue concurrently during finalization, but finalization would also have canceled our
    // write/close promise. The helper function also helpfully returns a reference to the current
    // request in flight.
    //
    // We capture the current generation and verify it hasn't changed, rather than using pointer
    // comparison, because RingBuffer may relocate elements when it grows.

    return [this, expectedGeneration = queue.currentGeneration()]<typename Request>() -> Request& {
      if constexpr (kj::isSameType<Request, Write>() || kj::isSameType<Request, Flush>()) {
        // Write and flush requests can have any number of requests backed up after them.
        KJ_ASSERT(!queue.empty());
      } else if constexpr (kj::isSameType<Request, Close>()) {
        // Pipe and Close requests are always the last one in the queue.
        KJ_ASSERT(queue.size() == 1, queue.size(), inspectQueue(queue, "Pipe"));
      } else if constexpr (kj::isSameType<Request, Pipe>()) {
        // Pipe and Close requests are always the last one in the queue.
        KJ_ASSERT(queue.size() == 1, queue.size(), inspectQueue(queue, "Pipe"));
      }

      // Verify nothing was popped from the queue while we were waiting.
      KJ_ASSERT(queue.currentGeneration() == expectedGeneration);

      return *queue.front().event.get<kj::Own<Request>>();
    };
  };

  const auto maybeAbort = [this](jsg::Lock& js) -> bool {
    auto& writable = KJ_ASSERT_NONNULL(state.tryGetUnsafe<IoOwn<Writable>>());
    KJ_IF_SOME(pendingAbort, WritableStreamController::PendingAbort::dequeue(maybePendingAbort)) {
      auto ex = js.exceptionToKj(pendingAbort->reason.addRef(js));
      writable->abort(kj::mv(ex));
      drain(js, pendingAbort->reason.getHandle(js));
      pendingAbort->complete(js);
      return true;
    }
    return false;
  };

  // Do we have anything left to do?
  if (queue.empty()) return js.resolvedPromise();

  KJ_SWITCH_ONEOF(queue.front().event) {
    KJ_CASE_ONEOF(request, kj::Own<Write>) {
      if (request->bytes.size() == 0) {
        // Zero-length writes are no-ops with a pending event. If we allowed them, we'd have a hard
        // time distinguishing between disconnections and zero-length reads on the other end of the
        // TransformStream.
        maybeResolvePromise(js, request->promise);
        queue.pop_front();

        // Note: we don't bother checking for an abort() here because either this write was just
        //   queued, in which case abort() cannot have been called yet, or this write was processed
        //   immediately after a previous write, in which case we just checked for an abort().
        return writeLoop(js, ioContext);
      }

      // writeLoop() is only called with the sink in the Writable state.
      auto& writable = state.getUnsafe<IoOwn<Writable>>();
      auto check = makeChecker();

      auto amountToWrite = request->bytes.size();

      auto promise = writable->sink->write(request->bytes).attach(kj::mv(request->ownBytes));

      // TODO(soon): We use awaitIoLegacy() here because if the stream terminates in JavaScript in
      // this same isolate, then the promise may actually be waiting on JavaScript to do something,
      // and so should not be considered waiting on external I/O. We will need to use
      // registerPendingEvent() manually when reading from an external stream. Ideally, we would
      // refactor the implementation so that when waiting on a JavaScript stream, we strictly use
      // jsg::Promises and not kj::Promises, so that it doesn't look like I/O at all, and there's
      // no need to drop the isolate lock and take it again every time some data is read/written.
      // That's a larger refactor, though.
      return ioContext.awaitIoLegacy(js, writable->canceler.wrap(kj::mv(promise)))
          .then(js,
              ioContext.addFunctor(
                  [this, check, maybeAbort, amountToWrite](jsg::Lock& js) -> jsg::Promise<void> {
        // Under some conditions, the clean up has already happened.
        if (queue.empty()) return js.resolvedPromise();
        auto& request = check.template operator()<Write>();
        maybeResolvePromise(js, request.promise);
        adjustWriteBufferSize(js, -amountToWrite);
        KJ_IF_SOME(o, observer) {
          o->onChunkDequeued(amountToWrite);
        }
        queue.pop_front();
        maybeAbort(js);
        return writeLoop(js, IoContext::current());
      }),
              ioContext.addFunctor([this, check, maybeAbort, amountToWrite](
                                       jsg::Lock& js, jsg::Value reason) -> jsg::Promise<void> {
        // Under some conditions, the clean up has already happened.
        if (queue.empty()) return js.resolvedPromise();
        auto handle = reason.getHandle(js);
        auto& request = check.template operator()<Write>();
        auto& writable = state.getUnsafe<IoOwn<Writable>>();
        adjustWriteBufferSize(js, -amountToWrite);
        KJ_IF_SOME(o, observer) {
          o->onChunkDequeued(amountToWrite);
        }
        maybeRejectPromise<void>(js, request.promise, handle);
        queue.pop_front();
        if (!maybeAbort(js)) {
          auto ex = js.exceptionToKj(reason.addRef(js));
          writable->abort(kj::mv(ex));
          drain(js, handle);
        }
        return js.resolvedPromise();
      }));
    }
    KJ_CASE_ONEOF(request, kj::Own<Pipe>) {
      // The destination should still be Writable, because the only way to transition to an
      // errored state would have been if a write request in the queue ahead of us encountered an
      // error. But in that case, the queue would already have been drained and we wouldn't be here.
      auto& writable = state.getUnsafe<IoOwn<Writable>>();

      if (request->checkSignal(js)) {
        // If the signal is triggered, checkSignal will handle erroring the source and destination.
        return js.resolvedPromise();
      }

      // The readable side should *should* still be readable here but let's double check, just
      // to be safe, both for closed state and errored states.
      if (request->source().isClosed()) {
        request->source().release(js);
        // If the source is closed, the spec requires us to close the destination unless the
        // preventClose option is true.
        if (!request->preventClose() && !isClosedOrClosing()) {
          doClose(js);
        } else {
          writeState.transitionTo<Unlocked>();
        }
        return js.resolvedPromise();
      }

      KJ_IF_SOME(errored, request->source().tryGetErrored(js)) {
        request->source().release(js);
        // If the source is errored, the spec requires us to error the destination unless the
        // preventAbort option is true.
        if (!request->preventAbort()) {
          auto ex = js.exceptionToKj(js.v8Ref(errored));
          writable->abort(kj::mv(ex));
          drain(js, errored);
        } else {
          writeState.transitionTo<Unlocked>();
        }
        return js.resolvedPromise();
      }

      // Up to this point, we really don't know what kind of ReadableStream source we're dealing
      // with. If the source is backed by a ReadableStreamSource, then the call to tryPumpTo below
      // will return a kj::Promise that will be resolved once the kj mechanisms for piping have
      // completed. From there, the only thing left to do is resolve the JavaScript pipe promise,
      // unlock things, and continue on. If the call to tryPumpTo returns nullptr, however, the
      // ReadableStream is JavaScript-backed and we need to setup a JavaScript-promise read/write
      // loop to pass the data into the destination.

      const auto handlePromise = [this, &ioContext, check = makeChecker(),
                                     preventAbort = request->preventAbort()](
                                     jsg::Lock& js, auto promise) {
        return promise.then(js, ioContext.addFunctor([this, check](jsg::Lock& js) mutable {
          // Under some conditions, the clean up has already happened.
          if (queue.empty()) return js.resolvedPromise();

          auto& request = check.template operator()<Pipe>();

          // It's possible we got here because the source errored but preventAbort was set.
          // In that case, we need to treat preventAbort the same as preventClose. Be
          // sure to check this before calling sourceLock.close() or the error detail will
          // be lost.
          // Capture preventClose now so we can modify it locally if needed.
          bool preventClose = request.preventClose();
          KJ_IF_SOME(errored, request.source().tryGetErrored(js)) {
            if (request.preventAbort()) preventClose = true;
            // Even through we're not going to close the destination, we still want the
            // pipe promise itself to be rejected in this case.
            maybeRejectPromise<void>(js, request.promise(), errored);
          } else KJ_IF_SOME(errored, state.tryGetUnsafe<StreamStates::Errored>()) {
            maybeRejectPromise<void>(js, request.promise(), errored.getHandle(js));
          } else {
            maybeResolvePromise(js, request.promise());
          }

          // Always transition the readable side to the closed state, because we read until EOF.
          // Note that preventClose (below) means "don't close the writable side", i.e. don't
          // call end().
          request.source().close(js);
          queue.pop_front();

          if (!preventClose) {
            // Note: unlike a real Close request, it's not possible for us to have been aborted.
            return close(js, true);
          } else {
            writeState.transitionTo<Unlocked>();
          }
          return js.resolvedPromise();
        }),
            ioContext.addFunctor(
                [this, check, preventAbort](jsg::Lock& js, jsg::Value reason) mutable {
          auto handle = reason.getHandle(js);
          auto& request = check.template operator()<Pipe>();
          maybeRejectPromise<void>(js, request.promise(), handle);
          // TODO(conform): Remember all those checks we performed in ReadableStream::pipeTo()?
          // We're supposed to perform the same checks continually, e.g., errored writes should
          // cancel the readable side unless preventCancel is truthy... This would require
          // deeper integration with the implementation of pumpTo(). Oh well. One consequence
          // of this is that if there is an error on the writable side, we error the readable
          // side, rather than close (cancel) it, which is what the spec would have us do.
          // TODO(now): Warn on the console about this.
          request.source().error(js, handle);
          queue.pop_front();
          if (!preventAbort) {
            return abort(js, handle);
          }
          doError(js, handle);
          return js.resolvedPromise();
        }));
      };

      KJ_IF_SOME(promise, request->source().tryPumpTo(*writable->sink, !request->preventClose())) {
        return handlePromise(js,
            ioContext.awaitIo(js,
                writable->canceler.wrap(
                    AbortSignal::maybeCancelWrap(js, request->maybeSignal(), kj::mv(promise)))));
      }

      // The ReadableStream is JavaScript-backed. We can still pipe the data but it's going to be
      // a bit slower because we will be relying on JavaScript promises when reading the data
      // from the ReadableStream, then waiting on kj::Promises to write the data. We will keep
      // reading until either the source or destination errors or until the source signals that
      // it is done.
      return handlePromise(js, request->pipeLoop(js));
    }
    KJ_CASE_ONEOF(request, kj::Own<Close>) {
      // writeLoop() is only called with the sink in the Writable state.
      auto& writable = state.getUnsafe<IoOwn<Writable>>();
      auto check = makeChecker();

      return ioContext.awaitIo(js, writable->canceler.wrap(writable->sink->end()))
          .then(js, ioContext.addFunctor([this, check](jsg::Lock& js) {
        // Under some conditions, the clean up has already happened.
        if (queue.empty()) return;
        auto& request = check.template operator()<Close>();
        maybeResolvePromise(js, request.promise);
        queue.pop_front();
        finishClose(js);
      }),
              ioContext.addFunctor([this, check](jsg::Lock& js, jsg::Value reason) {
        // Under some conditions, the clean up has already happened.
        if (queue.empty()) return;
        auto handle = reason.getHandle(js);
        auto& request = check.template operator()<Close>();
        maybeRejectPromise<void>(js, request.promise, handle);
        queue.pop_front();
        finishError(js, handle);
      }));
    }
    KJ_CASE_ONEOF(request, kj::Own<Flush>) {
      // This is not a standards-defined state for a WritableStream and is only used internally
      // for Socket's startTls call.
      //
      // Flushing is similar to closing the stream, the main difference is that `finishClose`
      // and `writable->end()` are never called.
      // Note: For Flush, we don't need makeChecker since we process immediately without async I/O.
      maybeResolvePromise(js, request->promise);
      queue.pop_front();

      return js.resolvedPromise();
    }
  }

  KJ_UNREACHABLE;
}

bool WritableStreamInternalController::Pipe::State::checkSignal(jsg::Lock& js) {
  // Returns true if the caller should bail out and stop processing. This happens in two cases:
  // 1. The State was aborted (e.g., by drain()) - the Pipe is being torn down
  // 2. The AbortSignal was triggered - we handle the abort and return true
  // In both cases, the caller should return a resolved promise and not continue the pipe loop.
  if (aborted) return true;

  KJ_IF_SOME(signal, maybeSignal) {
    if (signal->getAborted(js)) {
      auto reason = signal->getReason(js);

      // abort process might call parent.drain which will delete this,
      // move/copy everything we need after into temps.
      auto& parentRef = this->parent;
      auto& sourceRef = this->source;
      auto preventCancelCopy = this->preventCancel;
      auto promiseCopy = kj::mv(this->promise);

      if (!preventAbort) {
        KJ_IF_SOME(writable, parent.state.tryGetUnsafe<IoOwn<Writable>>()) {
          auto ex = js.exceptionToKj(reason);
          writable->abort(kj::mv(ex));
          parentRef.drain(js, reason);
        } else {
          parent.writeState.transitionTo<Unlocked>();
        }
      } else {
        parent.writeState.transitionTo<Unlocked>();
      }
      if (!preventCancelCopy) {
        sourceRef.release(js, v8::Local<v8::Value>(reason));
      } else {
        sourceRef.release(js);
      }
      maybeRejectPromise<void>(js, promiseCopy, reason);
      return true;
    }
  }
  return false;
}

jsg::Promise<void> WritableStreamInternalController::Pipe::State::write(
    v8::Local<v8::Value> handle) {
  auto& writable = parent.state.getUnsafe<IoOwn<Writable>>();
  // TODO(soon): Once jsg::BufferSource lands and we're able to use it, this can be simplified.
  KJ_ASSERT(handle->IsArrayBuffer() || handle->IsArrayBufferView());
  std::shared_ptr<v8::BackingStore> store;
  size_t byteLength = 0;
  size_t byteOffset = 0;
  if (handle->IsArrayBuffer()) {
    auto buffer = handle.template As<v8::ArrayBuffer>();
    store = buffer->GetBackingStore();
    byteLength = buffer->ByteLength();
  } else {
    auto view = handle.template As<v8::ArrayBufferView>();
    store = view->Buffer()->GetBackingStore();
    byteLength = view->ByteLength();
    byteOffset = view->ByteOffset();
  }
  kj::byte* data = reinterpret_cast<kj::byte*>(store->Data()) + byteOffset;
  // TODO(cleanup): Have this method accept a jsg::Lock& from the caller instead of using
  // v8::Isolate::GetCurrent();
  auto& js = jsg::Lock::current();
  return IoContext::current().awaitIo(js,
      writable->canceler.wrap(writable->sink->write(kj::arrayPtr(data, byteLength)))
          .attach(js.v8Ref(v8::ArrayBuffer::New(js.v8Isolate, store))),
      [](jsg::Lock&) {});
}

jsg::Promise<void> WritableStreamInternalController::Pipe::State::pipeLoop(jsg::Lock& js) {
  // This is a bit of dance. We got here because the source ReadableStream does not support
  // the internal, more efficient kj pipe (which means it is a JavaScript-backed ReadableStream).
  // We need to call read() on the source which returns a JavaScript Promise, wait on it to resolve,
  // then call write() which returns a kj::Promise. Before each iteration we check to see if either
  // the source or the destination have errored or closed and handle accordingly. At some point we
  // should explore if there are ways of making this more efficient. For the most part, however,
  // every read from the source must call into JavaScript to advance the ReadableStream.

  auto& ioContext = IoContext::current();

  if (aborted) {
    return js.resolvedPromise();
  }

  if (checkSignal(js)) {
    // If the signal is triggered, checkSignal will handle erroring the source and destination.
    return js.resolvedPromise();
  }

  // Here we check the closed and errored states of both the source and the destination,
  // propagating those states to the other based on the options. This check must be
  // performed at the start of each iteration in the pipe loop.
  //
  // TODO(soon): These are the same checks made before we entered the loop. Try to
  // unify the code to reduce duplication.

  KJ_IF_SOME(errored, source.tryGetErrored(js)) {
    source.release(js);
    if (!preventAbort) {
      KJ_IF_SOME(writable, parent.state.tryGetUnsafe<IoOwn<Writable>>()) {
        auto ex = js.exceptionToKj(js.v8Ref(errored));
        writable->abort(kj::mv(ex));
        return js.rejectedPromise<void>(errored);
      }
    }

    // If preventAbort was true, we're going to unlock the destination now.
    // We are not going to propagate the error here tho.
    parent.writeState.transitionTo<Unlocked>();
    return js.resolvedPromise();
  }

  KJ_IF_SOME(errored, parent.state.tryGetUnsafe<StreamStates::Errored>()) {
    parent.writeState.transitionTo<Unlocked>();
    if (!preventCancel) {
      auto reason = errored.getHandle(js);
      source.release(js, reason);
      return js.rejectedPromise<void>(reason);
    }
    source.release(js);
    return js.resolvedPromise();
  }

  if (source.isClosed()) {
    source.release(js);
    if (!preventClose) {
      KJ_ASSERT(!parent.state.is<StreamStates::Errored>());
      if (!parent.isClosedOrClosing()) {
        // We'll only be here if the sink is in the Writable state.
        auto& ioContext = IoContext::current();
        // Capture a ref to the state to keep it alive during async operations.
        return ioContext
            .awaitIo(js, parent.state.getUnsafe<IoOwn<Writable>>()->sink->end(), [](jsg::Lock&) {})
            .then(js, ioContext.addFunctor([state = kj::addRef(*this)](jsg::Lock& js) {
          if (state->aborted) return;
          state->parent.finishClose(js);
        }),
                ioContext.addFunctor([state = kj::addRef(*this)](jsg::Lock& js, jsg::Value reason) {
          if (state->aborted) return;
          state->parent.finishError(js, reason.getHandle(js));
        }));
      }
      parent.writeState.transitionTo<Unlocked>();
    }
    return js.resolvedPromise();
  }

  if (parent.isClosedOrClosing()) {
    auto destClosed = js.v8TypeError("This destination writable stream is closed."_kj);
    parent.writeState.transitionTo<Unlocked>();

    if (!preventCancel) {
      source.release(js, destClosed);
    } else {
      source.release(js);
    }

    return js.rejectedPromise<void>(destClosed);
  }

  return source.read(js).then(js,
      ioContext.addFunctor([state = kj::addRef(*this)](
                               jsg::Lock& js, ReadResult result) mutable -> jsg::Promise<void> {
    if (state->aborted || state->checkSignal(js) || result.done) {
      return js.resolvedPromise();
    }

    // WritableStreamInternalControllers only support byte data. If we can't
    // interpret the result.value as bytes, then we error the pipe; otherwise
    // we sent those bytes on to the WritableStreamSink.
    KJ_IF_SOME(value, result.value) {
      auto handle = value.getHandle(js);
      if (handle->IsArrayBuffer() || handle->IsArrayBufferView()) {
        return state->write(handle).then(js,
            [state = kj::addRef(*state)](jsg::Lock& js) mutable -> jsg::Promise<void> {
          if (state->aborted) {
            return js.resolvedPromise();
          }
          // The signal will be checked again at the start of the next loop iteration.
          return state->pipeLoop(js);
        },
            [state = kj::addRef(*state)](
                jsg::Lock& js, jsg::Value reason) mutable -> jsg::Promise<void> {
          if (state->aborted) {
            return js.resolvedPromise();
          }
          state->parent.doError(js, reason.getHandle(js));
          return state->pipeLoop(js);
        });
      }
    }
    // Undefined and null are perfectly valid values to pass through a ReadableStream,
    // but we can't interpret them as bytes so if we get them here, we error the pipe.
    auto error = js.v8TypeError("This WritableStream only supports writing byte types."_kj);
    auto& writable = state->parent.state.getUnsafe<IoOwn<Writable>>();
    auto ex = js.exceptionToKj(js.v8Ref(error));
    writable->abort(kj::mv(ex));
    // The error condition will be handled at the start of the next iteration.
    return state->pipeLoop(js);
  }),
      ioContext.addFunctor([state = kj::addRef(*this)](
                               jsg::Lock& js, jsg::Value reason) mutable -> jsg::Promise<void> {
    if (state->aborted) {
      return js.resolvedPromise();
    }
    // The error will be processed and propagated in the next iteration.
    return state->pipeLoop(js);
  }));
}

void WritableStreamInternalController::drain(jsg::Lock& js, v8::Local<v8::Value> reason) {
  doError(js, reason);
  while (!queue.empty()) {
    KJ_SWITCH_ONEOF(queue.front().event) {
      KJ_CASE_ONEOF(writeRequest, kj::Own<Write>) {
        maybeRejectPromise<void>(js, writeRequest->promise, reason);
      }
      KJ_CASE_ONEOF(pipeRequest, kj::Own<Pipe>) {
        if (!pipeRequest->preventCancel()) {
          pipeRequest->source().cancel(js, reason);
        }
        maybeRejectPromise<void>(js, pipeRequest->promise(), reason);
      }
      KJ_CASE_ONEOF(closeRequest, kj::Own<Close>) {
        maybeRejectPromise<void>(js, closeRequest->promise, reason);
      }
      KJ_CASE_ONEOF(flushRequest, kj::Own<Flush>) {
        maybeRejectPromise<void>(js, flushRequest->promise, reason);
      }
    }
    queue.pop_front();
  }
}

void WritableStreamInternalController::visitForGc(jsg::GcVisitor& visitor) {
  for (auto& event: queue) {
    KJ_SWITCH_ONEOF(event.event) {
      KJ_CASE_ONEOF(write, kj::Own<Write>) {
        visitor.visit(write->promise);
      }
      KJ_CASE_ONEOF(close, kj::Own<Close>) {
        visitor.visit(close->promise);
      }
      KJ_CASE_ONEOF(flush, kj::Own<Flush>) {
        visitor.visit(flush->promise);
      }
      KJ_CASE_ONEOF(pipe, kj::Own<Pipe>) {
        visitor.visit(pipe->maybeSignal(), pipe->promise());
      }
    }
  }
  KJ_IF_SOME(locked, writeState.tryGetUnsafe<WriterLocked>()) {
    visitor.visit(locked);
  }
  KJ_IF_SOME(pendingAbort, maybePendingAbort) {
    visitor.visit(*pendingAbort);
  }
}

void ReadableStreamInternalController::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(locked, readState.tryGetUnsafe<ReaderLocked>()) {
    visitor.visit(locked);
  }
}

kj::Maybe<ReadableStreamController::PipeController&> ReadableStreamInternalController::
    tryPipeLock() {
  if (isLockedToReader()) {
    return kj::none;
  }
  return readState.transitionTo<PipeLocked>(*this);
}

bool ReadableStreamInternalController::PipeLocked::isClosed() {
  return inner.state.is<StreamStates::Closed>();
}

kj::Maybe<v8::Local<v8::Value>> ReadableStreamInternalController::PipeLocked::tryGetErrored(
    jsg::Lock& js) {
  KJ_IF_SOME(errored, inner.state.tryGetUnsafe<StreamStates::Errored>()) {
    return errored.getHandle(js);
  }
  return kj::none;
}

void ReadableStreamInternalController::PipeLocked::cancel(
    jsg::Lock& js, v8::Local<v8::Value> reason) {
  if (inner.state.is<Readable>()) {
    inner.doCancel(js, reason);
  }
}

void ReadableStreamInternalController::PipeLocked::close(jsg::Lock& js) {
  inner.doClose(js);
}

void ReadableStreamInternalController::PipeLocked::error(
    jsg::Lock& js, v8::Local<v8::Value> reason) {
  inner.doError(js, reason);
}

void ReadableStreamInternalController::PipeLocked::release(
    jsg::Lock& js, kj::Maybe<v8::Local<v8::Value>> maybeError) {
  KJ_IF_SOME(error, maybeError) {
    cancel(js, error);
  }
  inner.readState.transitionTo<Unlocked>();
}

kj::Maybe<kj::Promise<void>> ReadableStreamInternalController::PipeLocked::tryPumpTo(
    WritableStreamSink& sink, bool end) {
  // This is safe because the caller should have already checked isClosed and tryGetErrored
  // and handled those before calling tryPumpTo.
  auto& readable = KJ_ASSERT_NONNULL(inner.state.tryGetUnsafe<Readable>());
  return IoContext::current().waitForDeferredProxy(readable->pumpTo(sink, end));
}

jsg::Promise<ReadResult> ReadableStreamInternalController::PipeLocked::read(jsg::Lock& js) {
  return KJ_ASSERT_NONNULL(inner.read(js, kj::none));
}

jsg::Promise<jsg::BufferSource> ReadableStreamInternalController::readAllBytes(
    jsg::Lock& js, uint64_t limit) {
  if (isLockedToReader()) {
    return js.rejectedPromise<jsg::BufferSource>(KJ_EXCEPTION(
        FAILED, "jsg.TypeError: This ReadableStream is currently locked to a reader."));
  }
  if (isPendingClosure) {
    return js.rejectedPromise<jsg::BufferSource>(
        js.v8TypeError("This ReadableStream belongs to an object that is closing."_kj));
  }
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 0);
      return js.resolvedPromise(jsg::BufferSource(js, kj::mv(backing)));
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<jsg::BufferSource>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(readable, Readable) {
      auto source = KJ_ASSERT_NONNULL(removeSource(js));
      auto& context = IoContext::current();
      // TODO(perf): v8 sandboxing will require that backing stores are allocated within
      // the sandbox. This will require a change to the API of ReadableStreamSource::readAllBytes.
      // For now, we'll read and allocate into a proper backing store.
      return context.awaitIoLegacy(js, source->readAllBytes(limit).attach(kj::mv(source)))
          .then(js, [](jsg::Lock& js, kj::Array<kj::byte> bytes) -> jsg::BufferSource {
        auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, bytes.size());
        backing.asArrayPtr().copyFrom(bytes);
        return jsg::BufferSource(js, kj::mv(backing));
      });
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<kj::String> ReadableStreamInternalController::readAllText(
    jsg::Lock& js, uint64_t limit) {
  if (isLockedToReader()) {
    return js.rejectedPromise<kj::String>(KJ_EXCEPTION(
        FAILED, "jsg.TypeError: This ReadableStream is currently locked to a reader."));
  }
  if (isPendingClosure) {
    return js.rejectedPromise<kj::String>(
        js.v8TypeError("This ReadableStream belongs to an object that is closing."_kj));
  }
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return js.resolvedPromise(kj::String());
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<kj::String>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(readable, Readable) {
      auto source = KJ_ASSERT_NONNULL(removeSource(js));
      auto& context = IoContext::current();
      auto option = ReadAllTextOption::NULL_TERMINATE;
      KJ_IF_SOME(flags, FeatureFlags::tryGet(js)) {
        if (flags.getStripBomInReadAllText()) {
          option |= ReadAllTextOption::STRIP_BOM;
        }
      }
      return context.awaitIoLegacy(js, source->readAllText(limit, option).attach(kj::mv(source)));
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<uint64_t> ReadableStreamInternalController::tryGetLength(StreamEncoding encoding) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return static_cast<uint64_t>(0);
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return kj::none;
    }
    KJ_CASE_ONEOF(readable, Readable) {
      return readable->tryGetLength(encoding);
    }
  }
  KJ_UNREACHABLE;
}

kj::Own<ReadableStreamController> ReadableStreamInternalController::detach(
    jsg::Lock& js, bool ignoreDetached) {
  return newReadableStreamInternalController(
      IoContext::current(), KJ_ASSERT_NONNULL(removeSource(js, ignoreDetached)));
}

kj::Promise<DeferredProxy<void>> ReadableStreamInternalController::pumpTo(
    jsg::Lock& js, kj::Own<WritableStreamSink> sink, bool end) {
  auto source = KJ_ASSERT_NONNULL(removeSource(js));

  struct Holder: public kj::Refcounted {
    kj::Own<WritableStreamSink> sink;
    kj::Own<ReadableStreamSource> source;
    bool done = false;

    Holder(kj::Own<WritableStreamSink> sink, kj::Own<ReadableStreamSource> source)
        : sink(kj::mv(sink)),
          source(kj::mv(source)) {}
    ~Holder() noexcept(false) {
      if (!done) {
        // It appears the pump was canceled. We should make sure this propagates back to the
        // source stream. This is important in particular when we're implementing the response
        // pump for an HTTP event (see Response::send()). Presumably it was canceled because the
        // client disconnected. If we don't cancel the source, then if the source is one end of
        // a TransformStream, the write end will just hang. Of course, this is fine if there are
        // no waitUntil()s running, because the whole I/O context will be canceled anyway. But if
        // there are waitUntil()s, then the application probably expects to get an exception from
        // the write() on cancellation, rather than have it hang.
        source->cancel(KJ_EXCEPTION(DISCONNECTED, "pump canceled"));
      }
    }
  };

  auto holder = kj::rc<Holder>(kj::mv(sink), kj::mv(source));
  return holder->source->pumpTo(*holder->sink, end)
      .then([holder = holder.addRef()](DeferredProxy<void> proxy) mutable -> DeferredProxy<void> {
    proxy.proxyTask = proxy.proxyTask.attach(holder.addRef());
    holder->done = true;
    return kj::mv(proxy);
  }, [holder = holder.addRef()](kj::Exception&& ex) mutable {
    holder->sink->abort(kj::cp(ex));
    holder->source->cancel(kj::cp(ex));
    holder->done = true;
    return kj::mv(ex);
  });
}

StreamEncoding ReadableStreamInternalController::getPreferredEncoding() {
  return state.tryGetUnsafe<Readable>()
      .map([](Readable& readable) {
    return readable->getPreferredEncoding();
  }).orDefault(StreamEncoding::IDENTITY);
}

kj::Own<ReadableStreamController> newReadableStreamInternalController(
    IoContext& ioContext, kj::Own<ReadableStreamSource> source) {
  return kj::heap<ReadableStreamInternalController>(ioContext.addObject(kj::mv(source)));
}

kj::Own<WritableStreamController> newWritableStreamInternalController(IoContext& ioContext,
    kj::Own<WritableStreamSink> sink,
    kj::Maybe<kj::Own<ByteStreamObserver>> observer,
    kj::Maybe<uint64_t> maybeHighWaterMark,
    kj::Maybe<jsg::Promise<void>> maybeClosureWaitable) {
  return kj::heap<WritableStreamInternalController>(
      kj::mv(sink), kj::mv(observer), maybeHighWaterMark, kj::mv(maybeClosureWaitable));
}

kj::StringPtr WritableStreamInternalController::jsgGetMemoryName() const {
  return "WritableStreamInternalController"_kjc;
}

size_t WritableStreamInternalController::jsgGetMemorySelfSize() const {
  return sizeof(WritableStreamInternalController);
}
void WritableStreamInternalController::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      tracker.trackField("error", errored);
    }
    KJ_CASE_ONEOF(_, IoOwn<Writable>) {
      // Ideally we'd be able to track the size of any pending writes held in the sink's
      // queue but since it is behind an IoOwn and we won't be holding the IoContext here,
      // we can't.
      tracker.trackFieldWithSize("IoOwn<WritableStreamSink>", sizeof(IoOwn<WritableStreamSink>));
    }
  }
  KJ_IF_SOME(writerLocked, writeState.tryGetUnsafe<WriterLocked>()) {
    tracker.trackField("writerLocked", writerLocked);
  }
  tracker.trackField("pendingAbort", maybePendingAbort);
  tracker.trackField("maybeClosureWaitable", maybeClosureWaitable);

  for (auto& event: queue) {
    tracker.trackField("event", event);
  }
}

kj::StringPtr ReadableStreamInternalController::jsgGetMemoryName() const {
  return "ReadableStreamInternalController"_kjc;
}

size_t ReadableStreamInternalController::jsgGetMemorySelfSize() const {
  return sizeof(ReadableStreamInternalController);
}

void ReadableStreamInternalController::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
    KJ_CASE_ONEOF(error, StreamStates::Errored) {
      tracker.trackField("error", error);
    }
    KJ_CASE_ONEOF(readable, Readable) {
      // Ideally we'd be able to track the size of any pending reads held in the source's
      // queue but since it is behind an IoOwn and we won't be holding the IoContext here,
      // we can't.
      tracker.trackFieldWithSize(
          "IoOwn<ReadableStreamSource>", sizeof(IoOwn<ReadableStreamSource>));
    }
  }
  KJ_SWITCH_ONEOF(readState) {
    KJ_CASE_ONEOF(unlocked, Unlocked) {}
    KJ_CASE_ONEOF(locked, Locked) {}
    KJ_CASE_ONEOF(pipeLocked, PipeLocked) {}
    KJ_CASE_ONEOF(readerLocked, ReaderLocked) {
      tracker.trackField("readerLocked", readerLocked);
    }
  }
}

}  // namespace workerd::api
