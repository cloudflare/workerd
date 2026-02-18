#include "stream-utils.h"

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/exception.h>
#include <kj/one-of.h>

namespace workerd {

namespace {

// An AsyncInputStream implementation that reads from an in-memory buffer.
// This is optimized for the case where the entire contents are available
// up-front, so it doesn't do any dynamic memory allocation or copying.
// It also supports optimized teeing when the backing storage is provided.
class MemoryInputStream final: public kj::AsyncInputStream {
 private:
  struct OwnedBacking: public kj::Refcounted {
    kj::Own<void> backing;
    OwnedBacking(kj::Own<void>&& backing): backing(kj::mv(backing)) {}
  };

 public:
  MemoryInputStream(
      kj::ArrayPtr<const kj::byte> data, kj::Maybe<kj::Own<void>> maybeBacking = kj::none)
      : data(data),
        // Note that we don't actually check that maybeBacking actually owns the
        // memory that `data` points to. It is the caller's responsibility to ensure
        // this is the case if they want teeing to be safely supported.
        ownedBacking(maybeBacking.map(
            [](kj::Own<void>& backing) mutable { return kj::rc<OwnedBacking>(kj::mv(backing)); })) {
  }
  MemoryInputStream(kj::ArrayPtr<const kj::byte> data, kj::Rc<OwnedBacking> ownedBacking)
      : data(data),
        ownedBacking(kj::mv(ownedBacking)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    auto ptr = kj::arrayPtr<kj::byte>(static_cast<kj::byte*>(buffer), maxBytes);
    size_t toRead = kj::min(data.size(), ptr.size());
    if (toRead == 0) return toRead;
    ptr.first(toRead).copyFrom(data.first(toRead));
    data = data.slice(toRead);
    return toRead;
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return data.size();
  }

  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    // An optimized pumpTo... we know we have all the data right here. We can
    // just write it all at once up to `amount`.
    uint64_t toRead = kj::min(data.size(), amount);
    if (toRead == 0) {
      co_return toRead;
    }
    co_await output.write(data.first(toRead));
    data = data.slice(toRead);
    co_return toRead;
  }

  kj::Maybe<kj::Own<AsyncInputStream>> tryTee(uint64_t limit = kj::maxValue) override {
    // If a MemoryInputStream is holding onto backing storage, then we can safely
    // tee it here, allowing us to avoid the default tee implementation which needs
    // additional buffering. Tee'ing just becomes a matter of sharing the backing
    // storage and the data slice directly. This allows us to avoid any additional
    // buffering in unread stream branches since all of the data is already in memory
    // anyway. If we're not holding onto the backing storage, then we cannot safely
    // assume that the a tee branch will safely be able to read the data, so we'll
    // fall back to the default kj::newTee implementation.
    KJ_IF_SOME(owned, ownedBacking) {
      return kj::heap<MemoryInputStream>(data, owned.addRef());
    }
    return kj::none;
  }

 private:
  kj::ArrayPtr<const kj::byte> data;
  kj::Maybe<kj::Rc<OwnedBacking>> ownedBacking;
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

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return canceler.wrap(getStream().write(buffer));
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

// The kj::NullStream instance is stateless, discards all writes, and returns
// EOF on all reads. We can, therefore, safely share a single static global
// instance instead of allocating a new one each time.
static kj::NullStream nullStream{};

}  // namespace

kj::AsyncOutputStream& getGlobalNullOutputStream() {
  return nullStream;
}

kj::Own<kj::AsyncIoStream> newNullIoStream() {
  return kj::Own<kj::AsyncIoStream>(&nullStream, kj::NullDisposer::instance);
}

kj::Own<kj::AsyncInputStream> newNullInputStream() {
  return kj::Own<kj::AsyncInputStream>(&nullStream, kj::NullDisposer::instance);
}

kj::Own<kj::AsyncOutputStream> newNullOutputStream() {
  return kj::Own<kj::AsyncOutputStream>(&nullStream, kj::NullDisposer::instance);
}

kj::Own<kj::AsyncInputStream> newMemoryInputStream(
    kj::ArrayPtr<const kj::byte> data, kj::Maybe<kj::Own<void>> maybeBacking) {
  return kj::heap<MemoryInputStream>(data, kj::mv(maybeBacking));
}

kj::Own<kj::AsyncInputStream> newMemoryInputStream(
    kj::StringPtr data, kj::Maybe<kj::Own<void>> maybeBacking) {
  return kj::heap<MemoryInputStream>(data.asBytes(), kj::mv(maybeBacking));
}

kj::Own<NeuterableInputStream> newNeuterableInputStream(kj::AsyncInputStream& inner) {
  return kj::refcounted<NeuterableInputStreamImpl>(inner);
}

kj::Own<NeuterableIoStream> newNeuterableIoStream(kj::AsyncIoStream& inner) {
  return kj::refcounted<NeuterableIoStreamImpl>(inner);
}

}  // namespace workerd
