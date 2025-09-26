#include "identity-transform-stream.h"

#include "common.h"

namespace workerd::api {

namespace {
// An implementation of ReadableStreamSource and WritableStreamSink which communicates read and
// write requests via a OneOf.
//
// This class is also used as the implementation of FixedLengthStream, in which case `limit` is
// non-nullptr.
class IdentityTransformStreamImpl final: public kj::Refcounted,
                                         public ReadableStreamSource,
                                         public WritableStreamSink {
 public:
  // The limit is the maximum number of bytes that can be fed through the stream.
  // If kj::none, there is no limit.
  explicit IdentityTransformStreamImpl(kj::Maybe<uint64_t> limit = kj::none): limit(limit) {}

  ~IdentityTransformStreamImpl() noexcept(false) {
    // Due to the different natures of JS and C++ disposal, there is no point in enforcing the limit
    // for a FixedLengthStream here.
    //
    // 1. Creating but not using a `new FixedLengthStream(n)` should not be an error, and ought not
    //    to logspam us.
    // 2. Chances are high that by the time this object gets destroyed, it's too late to tell the
    //    user about the failure.
  }

  // ReadableStreamSource implementation -------------------------------------------------

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    size_t total = 0;
    while (total < minBytes) {
      // TODO(perf): tryReadInternal was written assuming minBytes would always be 1 but we've now
      // introduced an API for user to specify a larger minBytes. For now, this is implemented as a
      // naive loop dispatching to the 1 byte version but would be better to bake it deeper into
      // the implementation where it can be more efficient.
      auto amount = co_await tryReadInternal(buffer, maxBytes);
      KJ_ASSERT(amount <= maxBytes);
      if (amount == 0) {
        // EOF.
        break;
      }

      total += amount;
      buffer = reinterpret_cast<char*>(buffer) + amount;
      maxBytes -= amount;
    }

    co_return total;
  }

  kj::Promise<size_t> tryReadInternal(void* buffer, size_t maxBytes) {
    auto promise = readHelper(kj::arrayPtr(static_cast<kj::byte*>(buffer), maxBytes));

    KJ_IF_SOME(l, limit) {
      promise = promise.then([this, &l = l](size_t amount) -> kj::Promise<size_t> {
        if (amount > l) {
          auto exception = JSG_KJ_EXCEPTION(
              FAILED, TypeError, "Attempt to write too many bytes through a FixedLengthStream.");
          cancel(exception);
          return kj::mv(exception);
        } else if (amount == 0 && l != 0) {
          auto exception = JSG_KJ_EXCEPTION(FAILED, TypeError,
              "FixedLengthStream did not see all expected bytes before close().");
          cancel(exception);
          return kj::mv(exception);
        }
        l -= amount;
        return amount;
      });
    }

    return promise;
  }

  kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end) override {
#ifdef KJ_NO_RTTI
    // Yes, I'm paranoid.
    static_assert(!KJ_NO_RTTI, "Need RTTI for correctness");
#endif

    // HACK: If `output` is another TransformStream, we don't allow pumping to it, in order to
    //   guarantee that we can't create cycles.
    JSG_REQUIRE(kj::dynamicDowncastIfAvailable<IdentityTransformStreamImpl>(output) == kj::none,
        TypeError, "Inter-TransformStream ReadableStream.pipeTo() is not implemented.");

    return ReadableStreamSource::pumpTo(output, end);
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    if (encoding == StreamEncoding::IDENTITY) {
      return limit;
    } else {
      return kj::none;
    }
  }

  void cancel(kj::Exception reason) override {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(idle, Idle) {
        // This is fine.
      }
      KJ_CASE_ONEOF(request, ReadRequest) {
        request.fulfiller->fulfill(size_t(0));
      }
      KJ_CASE_ONEOF(request, WriteRequest) {
        request.fulfiller->reject(kj::cp(reason));
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        // Already errored.
        return;
      }
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {
        // Already closed by writable side.
        return;
      }
    }

    state = kj::mv(reason);

    // TODO(conform): Proactively put WritableStream into Errored state.
  }

  // WritableStreamSink implementation ---------------------------------------------------

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    if (buffer == nullptr) {
      return kj::READY_NOW;
    }
    return writeHelper(buffer);
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    KJ_UNIMPLEMENTED("IdentityTransformStreamImpl piecewise write() not currently supported");
    // TODO(soon): This will be called by TeeBranch::pumpTo(). We disallow that anyway, since we
    //   disallow inter-TransformStream pumping.
  }

  kj::Promise<void> end() override {
    // If we're already closed, there's nothing else we need to do here.
    if (state.is<StreamStates::Closed>()) return kj::READY_NOW;

    return writeHelper(kj::ArrayPtr<const kj::byte>());
  }

  void abort(kj::Exception reason) override {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(idle, Idle) {
        // This is fine.
      }
      KJ_CASE_ONEOF(request, ReadRequest) {
        request.fulfiller->reject(kj::cp(reason));
      }
      KJ_CASE_ONEOF(request, WriteRequest) {
        // IF the fulfiller is not waiting, the write promise was already
        // canceled and no one is waiting on it.
        KJ_ASSERT(!request.fulfiller->isWaiting(),
            "abort() is supposed to wait for any pending write() to finish");
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        // Already errored.
        return;
      }
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {
        // If we're in the pending close state... it should be OK to just switch
        // the state to errored below.
      }
    }

    state = kj::mv(reason);

    // TODO(conform): Proactively put ReadableStream into Errored state.
  }

 private:
  kj::Promise<size_t> readHelper(kj::ArrayPtr<kj::byte> bytes) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(idle, Idle) {
        // No outstanding write request, switch to ReadRequest state.

        auto paf = kj::newPromiseAndFulfiller<size_t>();
        state = ReadRequest{bytes, kj::mv(paf.fulfiller)};
        return kj::mv(paf.promise);
      }
      KJ_CASE_ONEOF(request, ReadRequest) {
        KJ_FAIL_ASSERT("read operation already in flight");
      }
      KJ_CASE_ONEOF(request, WriteRequest) {
        if (bytes.size() >= request.bytes.size()) {
          // The write buffer will entirely fit into our read buffer; fulfill both requests.
          memcpy(bytes.begin(), request.bytes.begin(), request.bytes.size());
          auto result = request.bytes.size();
          request.fulfiller->fulfill();

          // Switch to idle state.
          state = Idle();

          return result;
        }

        // The write buffer won't quite fit into our read buffer; fulfill only the read request.
        memcpy(bytes.begin(), request.bytes.begin(), bytes.size());
        request.bytes = request.bytes.slice(bytes.size(), request.bytes.size());
        return bytes.size();
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        return kj::cp(exception);
      }
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {
        return size_t(0);
      }
    }

    KJ_UNREACHABLE;
  }

  kj::Promise<void> writeHelper(kj::ArrayPtr<const kj::byte> bytes) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(idle, Idle) {
        if (bytes.size() == 0) {
          // This is a close operation.
          state = StreamStates::Closed();
          return kj::READY_NOW;
        }

        auto paf = kj::newPromiseAndFulfiller<void>();
        state = WriteRequest{bytes, kj::mv(paf.fulfiller)};
        return kj::mv(paf.promise);
      }
      KJ_CASE_ONEOF(request, ReadRequest) {
        if (!request.fulfiller->isWaiting()) {
          // Oops, the request was canceled. Currently, this happen in particular when pumping a
          // response body to the client, and the client disconnects, cancelling the pump. In this
          // specific case, we want to propagate the error back to the write end of the transform
          // stream. In theory, though, there could be other cases where propagation is incorrect.
          //
          // TODO(cleanup): This cancellation should probably be handled at a higher level, e.g.
          //   in pumpTo(), but I need a quick fix.
          state = KJ_EXCEPTION(DISCONNECTED, "reader canceled");

          // I was going to use a `goto` but Harris choked on his bagel. Recursion it is.
          return writeHelper(bytes);
        }

        if (bytes.size() == 0) {
          // This is a close operation.
          request.fulfiller->fulfill(size_t(0));
          state = StreamStates::Closed();
          return kj::READY_NOW;
        }

        KJ_ASSERT(request.bytes.size() > 0);

        if (request.bytes.size() >= bytes.size()) {
          // Our write buffer will entirely fit into the read buffer; fulfill both requests.
          memcpy(request.bytes.begin(), bytes.begin(), bytes.size());
          request.fulfiller->fulfill(bytes.size());
          state = Idle();
          return kj::READY_NOW;
        }

        // Our write buffer won't quite fit into the read buffer; fulfill only the read request.
        memcpy(request.bytes.begin(), bytes.begin(), request.bytes.size());
        bytes = bytes.slice(request.bytes.size(), bytes.size());
        request.fulfiller->fulfill(request.bytes.size());

        auto paf = kj::newPromiseAndFulfiller<void>();
        state = WriteRequest{bytes, kj::mv(paf.fulfiller)};
        return kj::mv(paf.promise);
      }
      KJ_CASE_ONEOF(request, WriteRequest) {
        KJ_FAIL_ASSERT("write operation already in flight");
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        return kj::cp(exception);
      }
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {
        KJ_FAIL_ASSERT("close operation already in flight");
      }
    }

    KJ_UNREACHABLE;
  }

  kj::Maybe<uint64_t> limit;

  struct ReadRequest {
    kj::ArrayPtr<kj::byte> bytes;
    // WARNING: `bytes` may be invalid if fulfiller->isWaiting() returns false! (This indicates the
    //   read was canceled.)

    kj::Own<kj::PromiseFulfiller<size_t>> fulfiller;
  };

  struct WriteRequest {
    kj::ArrayPtr<const kj::byte> bytes;
    kj::Own<kj::PromiseFulfiller<void>> fulfiller;
  };

  struct Idle {};

  kj::OneOf<Idle, ReadRequest, WriteRequest, kj::Exception, StreamStates::Closed> state = Idle();
};
}  // namespace

jsg::Ref<IdentityTransformStream> IdentityTransformStream::constructor(
    jsg::Lock& js, jsg::Optional<IdentityTransformStream::QueuingStrategy> maybeQueuingStrategy) {

  auto& ioContext = IoContext::current();
  auto pipe = newIdentityPipe();

  kj::Maybe<uint64_t> maybeHighWaterMark = kj::none;
  KJ_IF_SOME(queuingStrategy, maybeQueuingStrategy) {
    maybeHighWaterMark = queuingStrategy.highWaterMark;
  }
  return js.alloc<IdentityTransformStream>(js.alloc<ReadableStream>(ioContext, kj::mv(pipe.in)),
      js.alloc<WritableStream>(ioContext, kj::mv(pipe.out),
          ioContext.getMetrics().tryCreateWritableByteStreamObserver(), maybeHighWaterMark));
}

jsg::Ref<FixedLengthStream> FixedLengthStream::constructor(jsg::Lock& js,
    uint64_t expectedLength,
    jsg::Optional<IdentityTransformStream::QueuingStrategy> maybeQueuingStrategy) {
  constexpr uint64_t MAX_SAFE_INTEGER = (1ull << 53) - 1;

  JSG_REQUIRE(expectedLength <= MAX_SAFE_INTEGER, TypeError,
      "FixedLengthStream requires an integer expected length less than 2^53.");

  auto& ioContext = IoContext::current();
  auto pipe = newIdentityPipe(expectedLength);

  kj::Maybe<uint64_t> maybeHighWaterMark = kj::none;
  // For a FixedLengthStream we do not want a highWaterMark higher than the expectedLength.
  KJ_IF_SOME(queuingStrategy, maybeQueuingStrategy) {
    maybeHighWaterMark = queuingStrategy.highWaterMark.map(
        [&](uint64_t highWaterMark) { return kj::min(expectedLength, highWaterMark); });
  }

  return js.alloc<FixedLengthStream>(js.alloc<ReadableStream>(ioContext, kj::mv(pipe.in)),
      js.alloc<WritableStream>(ioContext, kj::mv(pipe.out),
          ioContext.getMetrics().tryCreateWritableByteStreamObserver(), maybeHighWaterMark));
}

OneWayPipe newIdentityPipe(kj::Maybe<uint64_t> expectedLength) {
  auto readableSide = kj::refcounted<IdentityTransformStreamImpl>(expectedLength);
  auto writableSide = kj::addRef(*readableSide);
  return OneWayPipe{.in = kj::mv(readableSide), .out = kj::mv(writableSide)};
}

bool isIdentityTransformStream(WritableStreamSink& sink) {
  return kj::dynamicDowncastIfAvailable<IdentityTransformStreamImpl>(sink) != kj::none;
}

}  // namespace workerd::api
