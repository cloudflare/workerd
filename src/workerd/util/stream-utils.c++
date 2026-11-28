#include "stream-utils.h"

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/exception.h>
#include <kj/one-of.h>

namespace workerd {

namespace {

// An AsyncInputStream implementation that reads from an in-memory buffer.
// This is optimized for the case where the entire contents are available
// up-front, so it doesn't do any copying. The stream and its tee branches
// share a claim on the backing storage, so teeing needs no additional buffering.
class MemoryInputStream final: public kj::AsyncInputStream {
  using Bytes = kj::Rc<kj::ArrayPtr<const kj::byte>>;

 public:
  explicit MemoryInputStream(Bytes data): data(kj::mv(data)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return KJ_ASSERT_NONNULL(
        tryReadSync(kj::arrayPtr(static_cast<kj::byte*>(buffer), maxBytes), minBytes));
  }

  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    // All of the data is in memory, so every read can complete synchronously.
    size_t size = kj::min(data->size(), buffer.size());
    buffer.first(size).write(data->first(size));
    skip(size);
    return size;
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return data->size();
  }

  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    // An optimized pumpTo... we know we have all the data right here. We can
    // just write it all at once up to `amount`.
    size_t size = kj::min(data->size(), amount);
    if (size > 0) {
      auto bytes = data->first(size);
      if (!output.tryWriteSync(bytes)) {
        co_await output.write(bytes);
      }
    }
    skip(size);
    co_return size;
  }

  kj::Maybe<kj::Own<AsyncInputStream>> tryTee(uint64_t limit = kj::maxValue) override {
    // Teeing just shares the backing storage and the unread slice, avoiding the
    // buffering of the default kj::newTee implementation: all of the data is
    // already in memory anyway.
    return kj::heap<MemoryInputStream>(data.addRef());
  }

 private:
  Bytes data;

  void skip(size_t amount) {
    data = kj::mv(data).project([amount](auto& bytes) { return bytes.slice(amount); });
  }
};

class NeuterableInputStreamImpl final: public NeuterableInputStream {
 public:
  NeuterableInputStreamImpl(kj::AsyncInputStream& inner): inner(&inner) {}

  void neuter(kj::Exception exception) override {
    if (inner.is<kj::AsyncInputStream*>()) {
      canceler.cancel(inner.init<kj::Exception>(kj::mv(exception)));
    }
  }

  bool isNeutered() const override {
    return inner.is<kj::Exception>();
  }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return canceler.wrap(getStream().tryRead(buffer, minBytes, maxBytes));
  }
  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    KJ_IF_SOME(stream, inner.tryGet<kj::AsyncInputStream*>()) {
      return stream->tryReadSync(buffer, minBytes);
    }
    // Neutered; let the async path surface the exception.
    return kj::none;
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
        kj::throwException(exception.clone());
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
      canceler.cancel(inner.init<kj::Exception>(kj::mv(reason)));
    }
  }

  // AsyncInputStream

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return canceler.wrap(getStream().tryRead(buffer, minBytes, maxBytes));
  }
  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    KJ_IF_SOME(stream, inner.tryGet<kj::AsyncIoStream*>()) {
      return stream->tryReadSync(buffer, minBytes);
    }
    // Neutered; let the async path surface the exception.
    return kj::none;
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
  bool tryWriteSync(kj::ArrayPtr<const kj::byte> buffer) override {
    KJ_IF_SOME(stream, inner.tryGet<kj::AsyncIoStream*>()) {
      return stream->tryWriteSync(buffer);
    }
    // Neutered; let the async path surface the exception.
    return false;
  }
  bool tryWriteSync(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    KJ_IF_SOME(stream, inner.tryGet<kj::AsyncIoStream*>()) {
      return stream->tryWriteSync(pieces);
    }
    // Neutered; let the async path surface the exception.
    return false;
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
    kj::throwException(inner.get<kj::Exception>().clone());
  }
  kj::AsyncIoStream& getStream() const {
    KJ_IF_SOME(stream, inner.tryGet<kj::AsyncIoStream*>()) {
      return *stream;
    }
    kj::throwException(inner.get<kj::Exception>().clone());
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

kj::Own<kj::AsyncInputStream> newMemoryInputStream(kj::Rc<kj::ArrayPtr<const kj::byte>> data) {
  return kj::heap<MemoryInputStream>(kj::mv(data));
}

kj::Own<NeuterableInputStream> newNeuterableInputStream(kj::AsyncInputStream& inner) {
  return kj::refcounted<NeuterableInputStreamImpl>(inner);
}

kj::Rc<NeuterableIoStream> newNeuterableIoStream(kj::AsyncIoStream& inner) {
  return kj::rc<NeuterableIoStreamImpl>(inner);
}

}  // namespace workerd
