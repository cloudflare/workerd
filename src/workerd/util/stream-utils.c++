#include "stream-utils.h"
#include <kj/common.h>
#include <kj/exception.h>
#include <kj/one-of.h>
#include <kj/debug.h>

namespace workerd {

namespace {
class NullIoStream final: public kj::AsyncIoStream {
public:
  void shutdownWrite() override {}

  kj::Promise<void> write(const void* buffer, size_t size) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return kj::constPromise<size_t, 0>();
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return kj::Maybe<uint64_t>((uint64_t)0);
  }

  kj::Promise<uint64_t> pumpTo(AsyncOutputStream& output, uint64_t amount) override {
    return kj::constPromise<uint64_t, 0>();
  }
};

class MemoryInputStream final: public kj::AsyncInputStream {
public:
  MemoryInputStream(kj::ArrayPtr<const kj::byte> data)
      : data(data) { }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    size_t toRead = kj::min(data.size(), maxBytes);
    memcpy(buffer, data.begin(), toRead);
    data = data.slice(toRead, data.size());
    return toRead;
  }

private:
  kj::ArrayPtr<const kj::byte> data;
};

class NeuterableInputStreamImpl final: public NeuterableInputStream {
public:
  NeuterableInputStreamImpl(kj::AsyncInputStream& inner): inner(&inner) {}

  void neuter(kj::Exception exception) override {
    if (inner.is<kj::AsyncInputStream*>()) {
      inner = kj::cp(exception);
      if (!canceler.isEmpty()) {
        canceler.cancel(kj::mv(exception));
      }
    }
  }

  kj::Promise<size_t> read(void* buffer, size_t minBytes, size_t maxBytes) override {
    return canceler.wrap(getStream().read(buffer, minBytes, maxBytes));
  }
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return canceler.wrap(getStream().tryRead(buffer, minBytes, maxBytes));
  }
  kj::Maybe<uint64_t> tryGetLength() override {
    return getStream().tryGetLength();
  }
  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    return canceler.wrap(getStream().pumpTo(output, amount));
  }

private:
  kj::OneOf<kj::AsyncInputStream*, kj::Exception> inner;
  kj::Canceler canceler;

  kj::AsyncInputStream& getStream() {
    KJ_SWITCH_ONEOF(inner) {
      KJ_CASE_ONEOF(stream, kj::AsyncInputStream*) {
        return *stream;
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        kj::throwFatalException(kj::cp(exception));
      }
    }
    KJ_UNREACHABLE;
  }
};

class NeuterableIoStreamImpl final: public NeuterableIoStream {
public:
  NeuterableIoStreamImpl(kj::AsyncIoStream& inner): inner(&inner) {}

  void neuter(kj::Exception reason) override {
    if (inner.is<kj::AsyncIoStream*>()) {
      inner = kj::cp(reason);
      if (!canceler.isEmpty()) {
        canceler.cancel(kj::mv(reason));
      }
    }
  }

  // AsyncInputStream

  kj::Promise<size_t> read(void* buffer, size_t minBytes, size_t maxBytes) override {
    return canceler.wrap(getStream().read(buffer, minBytes, maxBytes));
  }
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return canceler.wrap(getStream().tryRead(buffer, minBytes, maxBytes));
  }
  kj::Maybe<uint64_t> tryGetLength() override {
    return getStream().tryGetLength();
  }
  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    return canceler.wrap(getStream().pumpTo(output, amount));
  }

  // AsyncOutputStream

  kj::Promise<void> write(const void* buffer, size_t size) override {
    return canceler.wrap(getStream().write(buffer, size));
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    return canceler.wrap(getStream().write(pieces));
  }
  kj::Maybe<kj::Promise<uint64_t>> tryPumpFrom(
      kj::AsyncInputStream& input, uint64_t amount) override {
    return getStream().tryPumpFrom(input, amount).map([this](kj::Promise<uint64_t> promise) {
      return canceler.wrap(kj::mv(promise));
    });
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return canceler.wrap(getStream().whenWriteDisconnected());
  }

  // AsyncIoStream

  void shutdownWrite() override {
    getStream().shutdownWrite();
  };
  void abortRead() override {
    getStream().abortRead();
  }
  void getsockopt(int level, int option, void* value, kj::uint* length) override {
    getStream().getsockopt(level, option, value, length);
  }
  void setsockopt(int level, int option, const void* value, kj::uint length) override {
    getStream().setsockopt(level, option, value, length);
  }
  void getsockname(struct sockaddr* addr, kj::uint* length) override {
    getStream().getsockname(addr, length);
  }
  void getpeername(struct sockaddr* addr, kj::uint* length) override {
    getStream().getpeername(addr, length);
  }
  virtual kj::Maybe<int> getFd() const override {
    return getStream().getFd();
  }

private:
  kj::OneOf<kj::AsyncIoStream*, kj::Exception> inner;
  kj::Canceler canceler;

  kj::AsyncIoStream& getStream() {
    KJ_IF_SOME(stream, inner.tryGet<kj::AsyncIoStream*>()) {
      return *stream;
    }
    kj::throwFatalException(kj::cp(inner.get<kj::Exception>()));
  }
  kj::AsyncIoStream& getStream() const {
    KJ_IF_SOME(stream, inner.tryGet<kj::AsyncIoStream*>()) {
      return *stream;
    }
    kj::throwFatalException(kj::cp(inner.get<kj::Exception>()));
  }
};

}  // namespace

kj::Own<kj::AsyncIoStream> newNullIoStream() {
  return kj::heap<NullIoStream>();
}

kj::Own<kj::AsyncInputStream> newNullInputStream() {
  return kj::heap<NullIoStream>();
}

kj::Own<kj::AsyncOutputStream> newNullOutputStream() {
  return kj::heap<NullIoStream>();
}

kj::Own<kj::AsyncInputStream> newMemoryInputStream(kj::ArrayPtr<const kj::byte> data) {
  return kj::heap<MemoryInputStream>(data);
}

kj::Own<kj::AsyncInputStream> newMemoryInputStream(kj::StringPtr data) {
  return kj::heap<MemoryInputStream>(data.asBytes());
}

kj::Own<NeuterableInputStream> newNeuterableInputStream(kj::AsyncInputStream& inner) {
  return kj::refcounted<NeuterableInputStreamImpl>(inner);
}

kj::Own<NeuterableIoStream> newNeuterableIoStream(kj::AsyncIoStream& inner) {
  return kj::refcounted<NeuterableIoStreamImpl>(inner);
}

}  // namespace workerd
