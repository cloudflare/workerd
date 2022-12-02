// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/async-io.h>
#include <kj/exception.h>
#include <kj/one-of.h>

namespace workerd::api {

using kj::uint;

class RevocableIoStream final: public kj::AsyncIoStream {
  // An AsyncIoStream that can be disconnected.
  //
  // TODO(cleanup): There's a NeuterableInputStream in api/global-scope.c++ that accomplishes
  //   something similar.

public:
  RevocableIoStream(kj::AsyncIoStream& inner): inner(&inner) {}

  void revoke(kj::Exception reason) {
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
  void getsockopt(int level, int option, void* value, uint* length) override {
    getStream().getsockopt(level, option, value, length);
  }
  void setsockopt(int level, int option, const void* value, uint length) override {
    getStream().setsockopt(level, option, value, length);
  }
  void getsockname(struct sockaddr* addr, uint* length) override {
    getStream().getsockname(addr, length);
  }
  void getpeername(struct sockaddr* addr, uint* length) override {
    getStream().getpeername(addr, length);
  }
  virtual kj::Maybe<int> getFd() const override {
    return getStream().getFd();
  }

private:
  kj::OneOf<kj::AsyncIoStream*, kj::Exception> inner;
  kj::Canceler canceler;

  kj::AsyncIoStream& getStream() {
    KJ_IF_MAYBE(stream, inner.tryGet<kj::AsyncIoStream*>()) {
      return **stream;
    }
    kj::throwFatalException(kj::cp(inner.get<kj::Exception>()));
  }
  kj::AsyncIoStream& getStream() const {
    KJ_IF_MAYBE(stream, inner.tryGet<kj::AsyncIoStream*>()) {
      return **stream;
    }
    kj::throwFatalException(kj::cp(inner.get<kj::Exception>()));
  }
};

}  // namespace edgeworker
