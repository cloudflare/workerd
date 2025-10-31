#include "writable-sink.h"

#include <workerd/io/io-context.h>
#include <workerd/util/stream-utils.h>

#include <capnp/compat/byte-stream.h>
#include <kj/async-io.h>
#include <kj/compat/brotli.h>
#include <kj/compat/gzip.h>
#include <kj/one-of.h>

namespace workerd::api::streams {

namespace {
struct Closed {};

// The base implementation of WritableSink. This is not exposed publicly.
class WritableSinkImpl: public WritableSink {
 public:
  WritableSinkImpl(kj::Own<kj::AsyncOutputStream> inner,
      rpc::StreamEncoding encoding = rpc::StreamEncoding::IDENTITY)
      : state(kj::mv(inner)),
        encoding(encoding) {}
  WritableSinkImpl(): state(Closed()), encoding(rpc::StreamEncoding::IDENTITY) {}
  WritableSinkImpl(kj::Exception reason)
      : state(kj::cp(reason)),
        encoding(rpc::StreamEncoding::IDENTITY) {}

  KJ_DISALLOW_COPY_AND_MOVE(WritableSinkImpl);

  virtual ~WritableSinkImpl() noexcept(false) {
    if (!canceler.isEmpty()) {
      canceler.cancel(KJ_EXCEPTION(DISCONNECTED, "stream was dropped"));
    }
  }

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override final {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(inner, kj::Own<kj::AsyncOutputStream>) {
        KJ_REQUIRE(canceler.isEmpty(), "jsg.Error: Stream is already being written to");
        try {
          co_return co_await canceler.wrap(encodeAndWrite(prepareWrite(kj::mv(inner)), buffer));
        } catch (...) {
          auto exception = kj::getCaughtExceptionAsKj();
          setErrored(kj::cp(exception));
          kj::throwFatalException(kj::mv(exception));
        }
      }
      KJ_CASE_ONEOF(closed, Closed) {
        JSG_FAIL_REQUIRE(Error, "Cannot write to a closed stream.");
      }
      KJ_CASE_ONEOF(errored, kj::Exception) {
        kj::throwFatalException(kj::cp(errored));
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override final {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(inner, kj::Own<kj::AsyncOutputStream>) {
        KJ_REQUIRE(canceler.isEmpty(), "jsg.Error: Stream is already being written to");
        try {
          co_return co_await canceler.wrap(encodeAndWrite(prepareWrite(kj::mv(inner)), pieces));
        } catch (...) {
          auto exception = kj::getCaughtExceptionAsKj();
          setErrored(kj::cp(exception));
          kj::throwFatalException(kj::mv(exception));
        }
      }
      KJ_CASE_ONEOF(closed, Closed) {
        JSG_FAIL_REQUIRE(Error, "Cannot write to a closed stream.");
      }
      KJ_CASE_ONEOF(errored, kj::Exception) {
        kj::throwFatalException(kj::cp(errored));
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Promise<void> end() override final {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(open, kj::Own<kj::AsyncOutputStream>) {
        KJ_REQUIRE(canceler.isEmpty(), "jsg.Error: Stream is already being written to");
        // The AsyncOutputStream interface does not yet have an end() method.
        // Instead, we just drop it, signaling EOF. Eventually, it might get
        // an end method, at which point we should use that instead.
        try {
          co_await canceler.wrap(endImpl(*open));
          setClosed();
          co_return;
        } catch (...) {
          auto exception = kj::getCaughtExceptionAsKj();
          setErrored(kj::cp(exception));
          kj::throwFatalException(kj::mv(exception));
        }
      }
      KJ_CASE_ONEOF(closed, Closed) {
        co_return;
      }
      KJ_CASE_ONEOF(errored, kj::Exception) {
        kj::throwFatalException(kj::cp(errored));
      }
    }
    KJ_UNREACHABLE;
  }

  void abort(kj::Exception reason) override final {
    canceler.cancel(kj::cp(reason));
    setErrored(kj::mv(reason));
  }

  rpc::StreamEncoding disownEncodingResponsibility() override final {
    auto prev = encoding;
    encoding = rpc::StreamEncoding::IDENTITY;
    return prev;
  }

  rpc::StreamEncoding getEncoding() override final {
    return encoding;
  }

 protected:
  virtual kj::AsyncOutputStream& prepareWrite(kj::Own<kj::AsyncOutputStream>&& inner) {
    return setStream(kj::mv(inner));
  };

  virtual kj::Promise<void> encodeAndWrite(
      kj::AsyncOutputStream& output, kj::ArrayPtr<const kj::byte> data) {
    co_await output.write(data);
  }

  virtual kj::Promise<void> encodeAndWrite(
      kj::AsyncOutputStream& output, kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) {
    co_await output.write(pieces);
  }

  virtual kj::Promise<void> endImpl(kj::AsyncOutputStream& output) {
    // When using the default implementation, we assume IDENTITY encoding.
    KJ_ASSERT(encoding == rpc::StreamEncoding::IDENTITY);
    if (auto endable = dynamic_cast<EndableAsyncOutputStream*>(&output)) {
      co_await endable->end();
    } else if (auto endable = dynamic_cast<capnp::ExplicitEndOutputStream*>(&output)) {
      co_await endable->end();
    }
    // By default there's nothing to flush.
    co_return;
  }

  void setClosed() {
    state.init<Closed>();
  }

  void setErrored(kj::Exception&& ex) {
    state = kj::cp(ex);
  }

  kj::AsyncOutputStream& setStream(kj::Own<kj::AsyncOutputStream> inner) {
    auto& ret = *inner;
    state = kj::mv(inner);
    return ret;
  }

  kj::OneOf<kj::Own<kj::AsyncOutputStream>, Closed, kj::Exception>& getState() {
    return state;
  }

 private:
  kj::OneOf<kj::Own<kj::AsyncOutputStream>, Closed, kj::Exception> state;
  rpc::StreamEncoding encoding;
  kj::Canceler canceler;
};

// A wrapper around a native `kj::AsyncOutputStream` which knows the underlying encoding of the
// stream and optimizes pumps from `EncodedAsyncInputStream`.
//
// The inner will be held on to right up until either end() or abort() is called.
// This is important because some AsyncOutputStream implementations perform cleanup
// operations equivalent to end() in their destructors (for instance HttpChunkedEntityWriter).
// If we wait to clear the kj::Own when the EncodedAsyncOutputStream is destroyed, and the
// EncodedAsyncOutputStream is owned (for instance) by an IoOwn, then the lifetime of the
// inner may be extended past when it should. Eventually, kj::AsyncOutputStream should
// probably have a distinct end() method of its own that we can defer to, but until it
// does, it is important for us to release it as soon as end() or abort() are called.
class EncodedAsyncOutputStream final: public WritableSinkImpl {
 public:
  explicit EncodedAsyncOutputStream(
      kj::Own<kj::AsyncOutputStream> inner, rpc::StreamEncoding encoding)
      : WritableSinkImpl(kj::mv(inner), encoding) {}

  kj::Promise<void> endImpl(kj::AsyncOutputStream& output) override {
    if (auto gzip = dynamic_cast<kj::GzipAsyncOutputStream*>(&output)) {
      co_await gzip->end();
    } else if (auto br = dynamic_cast<kj::BrotliAsyncOutputStream*>(&output)) {
      co_await br->end();
    } else if (auto endable = dynamic_cast<EndableAsyncOutputStream*>(&output)) {
      co_await endable->end();
    } else if (auto endable = dynamic_cast<capnp::ExplicitEndOutputStream*>(&output)) {
      co_await endable->end();
    }
    // By default there's nothing to flush.
  }

  kj::AsyncOutputStream& prepareWrite(kj::Own<kj::AsyncOutputStream>&& inner) override {
    switch (disownEncodingResponsibility()) {
      case rpc::StreamEncoding::GZIP: {
        return setStream(kj::heap<kj::GzipAsyncOutputStream>(*inner).attach(kj::mv(inner)));
      }
      case rpc::StreamEncoding::BROTLI: {
        return setStream(kj::heap<kj::BrotliAsyncOutputStream>(*inner).attach(kj::mv(inner)));
      }
      case rpc::StreamEncoding::IDENTITY: {
        return setStream(kj::mv(inner));
      }
    }
    KJ_UNREACHABLE;
  }
};

// A wrapper around a WritableSink that registers pending events with an IoContext.
class IoContextWritableSinkWrapper: public WritableSinkWrapper {
 public:
  IoContextWritableSinkWrapper(IoContext& ioContext, kj::Own<WritableSink> inner)
      : WritableSinkWrapper(kj::mv(inner)),
        ioContext(ioContext) {}

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    auto pending = ioContext.registerPendingEvent();
    KJ_IF_SOME(p, ioContext.waitForOutputLocksIfNecessary()) {
      co_await p;
    }
    co_await getInner().write(buffer);
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    auto pending = ioContext.registerPendingEvent();
    KJ_IF_SOME(p, ioContext.waitForOutputLocksIfNecessary()) {
      co_await p;
    }
    co_await getInner().write(pieces);
  }

  kj::Promise<void> end() override {
    auto pending = ioContext.registerPendingEvent();
    KJ_IF_SOME(p, ioContext.waitForOutputLocksIfNecessary()) {
      co_await p;
    }
    co_await getInner().end();
  }

 private:
  IoContext& ioContext;
};
}  // namespace

kj::Own<WritableSink> newWritableSink(kj::Own<kj::AsyncOutputStream> inner) {
  return kj::heap<WritableSinkImpl>(kj::mv(inner));
}

kj::Own<WritableSink> newClosedWritableSink() {
  return kj::heap<WritableSinkImpl>();
}

kj::Own<WritableSink> newErroredWritableSink(kj::Exception reason) {
  return kj::heap<WritableSinkImpl>(kj::mv(reason));
}

kj::Own<WritableSink> newNullWritableSink() {
  return kj::heap<WritableSinkImpl>(newNullOutputStream());
}

kj::Own<WritableSink> newEncodedWritableSink(
    rpc::StreamEncoding encoding, kj::Own<kj::AsyncOutputStream> inner) {
  return kj::heap<EncodedAsyncOutputStream>(kj::mv(inner), encoding);
}

kj::Own<WritableSink> newIoContextWrappedWritableSink(
    IoContext& ioContext, kj::Own<WritableSink> inner) {
  return kj::heap<IoContextWritableSinkWrapper>(ioContext, kj::mv(inner));
}

}  // namespace workerd::api::streams
