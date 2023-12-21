// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
#include "canceler.h"
#include <kj/compat/http.h>

namespace workerd {

template <typename T>
class AbortableImpl final {
public:
  AbortableImpl(kj::Own<T> inner, RefcountedCanceler& canceler)
      : canceler(kj::addRef(canceler)),
        inner(kj::mv(inner)),
        onCancel(*(this->canceler), [this]() { this->inner = kj::none; }) {}

  template <typename V, typename... Args, typename...ArgsT>
  kj::Promise<V> wrap(kj::Promise<V>(T::*fn)(ArgsT...), Args&&...args) {
    return wrap([&](T& inner) { return (inner.*fn)(kj::fwd<ArgsT>(args)...); });
  }

  template <typename Func>
  auto wrap(Func fn) -> decltype(fn(kj::instance<T&>())) {
    // Be aware that the getInner() here can throw synchronously if the
    // canceler has already been tripped.
    return canceler->wrap(fn(getInner()));
  }

  T& getInner() {
    canceler->throwIfCanceled();
    // If we get past throwIfCanceled successfully, inner should still
    // be set. If it's not, then we've got a bug somewhere and we need
    // to know about it.
    return *(KJ_ASSERT_NONNULL(inner));
  }

private:
  kj::Own<RefcountedCanceler> canceler;
  kj::Maybe<kj::Own<T>> inner;
  RefcountedCanceler::Listener onCancel;
};

// An InputStream that can be disconnected in response to RefcountedCanceler.
// This is similar to NeuterableInputStream in global-scope.c++ but uses an
// external kj::Canceler to trigger the disconnect.
// This is currently only used in fetch() requests that use an AbortSignal.
// The AbortableInputStream is created using a RefcountedCanceler,
// which will be triggered when the AbortSignal is triggered.
// TODO(later): It would be good to see if both this and NeuterableInputStream
// could be combined into a single utility.
class AbortableInputStream final: public kj::AsyncInputStream,
                                  public kj::Refcounted {
public:
  AbortableInputStream(kj::Own<kj::AsyncInputStream> inner, RefcountedCanceler& canceler)
      : impl(kj::mv(inner), canceler) {}

  kj::Promise<size_t> read(void* buffer, size_t minBytes, size_t maxBytes) override {
    return impl.wrap<size_t>(&kj::AsyncInputStream::read, buffer, minBytes, maxBytes);
  }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return impl.wrap(&kj::AsyncInputStream::tryRead, buffer, minBytes, maxBytes);
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return impl.getInner().tryGetLength();
  }

  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    return impl.wrap(&kj::AsyncInputStream::pumpTo, output, amount);
  }

private:
  AbortableImpl<kj::AsyncInputStream> impl;
};

// A WebSocket wrapper that can be disconnected in response to a RefcountedCanceler.
// This is currently only used when opening a WebSocket with a fetch() request that
// is using an AbortSignal. The AbortableWebSocket is created using the AbortSignal's
// RefcountedCanceler, which will be triggered when the AbortSignal is triggered.
class AbortableWebSocket final: public kj::WebSocket,
                                public kj::Refcounted {
public:
  AbortableWebSocket(kj::Own<kj::WebSocket> inner, RefcountedCanceler& canceler)
      : impl(kj::mv(inner), canceler) {}

  kj::Promise<void> send(kj::ArrayPtr<const kj::byte> message) override {
    return impl.wrap(
        static_cast<kj::Promise<void>(kj::WebSocket::*)(kj::ArrayPtr<const kj::byte>)>(
            &kj::WebSocket::send), message);
  }

  kj::Promise<void> send(kj::ArrayPtr<const char> message) override {
    return impl.wrap(
        static_cast<kj::Promise<void>(kj::WebSocket::*)(kj::ArrayPtr<const char>)>(
            &kj::WebSocket::send), message);
  }

  kj::Promise<void> close(uint16_t code, kj::StringPtr reason) override {
    return impl.wrap(&kj::WebSocket::close, code, reason);
  }

  kj::Promise<void> disconnect() override {
    return impl.wrap(&kj::WebSocket::disconnect);
  }

  void abort() override {
    impl.getInner().abort();
  }

  kj::Promise<void> whenAborted() override {
    return impl.wrap(&kj::WebSocket::whenAborted);
  }

  kj::Promise<Message> receive(size_t maxSize = SUGGESTED_MAX_MESSAGE_SIZE) override {
    return impl.wrap(&kj::WebSocket::receive, maxSize);
  }

  kj::Promise<void> pumpTo(kj::WebSocket& other) override {
    return impl.wrap(&kj::WebSocket::pumpTo, other);
  }

  kj::Maybe<kj::Promise<void>> tryPumpFrom(kj::WebSocket& other) override {
    return impl.wrap([&other](auto& inner) -> kj::Promise<void> { return other.pumpTo(inner); });
  }

  uint64_t sentByteCount() override {
    return impl.getInner().sentByteCount();
  }

  uint64_t receivedByteCount() override {
    return impl.getInner().receivedByteCount();
  }

  kj::Maybe<kj::String> getPreferredExtensions(ExtensionsContext ctx) override {
    return impl.getInner().getPreferredExtensions(ctx);
  };


private:
  AbortableImpl<kj::WebSocket> impl;
};

} // namespace workerd
