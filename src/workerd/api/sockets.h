// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include "http.h"


namespace workerd::api {

class NotifiedAsyncIoStream final : public kj::AsyncIoStream {
  // A stream that contains a callback that will be called whenever the stream's read side receives
  // an EOF.
public:
  explicit NotifiedAsyncIoStream(kj::Own<kj::AsyncIoStream> inner,
      kj::Function<void()> onCloseRead);
  void shutdownWrite() override;
  void abortRead() override;
  void getsockopt(int level, int option, void* value, uint* length) override;
  void setsockopt(int level, int option, const void* value, uint length) override;

  void getsockname(struct sockaddr* addr, uint* length) override;
  void getpeername(struct sockaddr* addr, uint* length) override;

  // AsyncInputStream methods:
  kj::Promise<size_t> read(void* buffer, size_t minBytes, size_t maxBytes) override;
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override;

  // AsyncOutputStream methods:
  kj::Promise<void> write(const void* buffer, size_t size) override;
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override;

  kj::Promise<void> whenWriteDisconnected() override;

private:
  kj::Own<kj::AsyncIoStream> inner;
  // A promise holding a stream that will be available at some point in the future. Typically once
  // a connection is established.
  kj::Function<void()> onCloseRead;
  // Callback used to notify the Socket of stream read of 0 bytes.
};

struct SocketOptions {
  bool tsl; // TODO(later): TCP socket options need to be implemented.
  JSG_STRUCT(tsl);
};

struct InitData {
  jsg::Ref<ReadableStream> readable;
  jsg::Ref<WritableStream> writable;
  IoOwn<jsg::PromiseResolverPair<void>> closeFulfiller;
};

class Socket: public jsg::Object {
public:
  Socket(jsg::Lock& js, kj::Promise<kj::Own<kj::AsyncIoStream>> connectionPromise);

  jsg::Ref<ReadableStream> getReadable() { return readable.addRef(); }
  jsg::Ref<WritableStream> getWritable() { return writable.addRef(); }
  jsg::MemoizedIdentity<jsg::Promise<void>>& getClosed() {
    return closedPromise;
  }

  void close();
  // Closes the socket connection.

  JSG_RESOURCE_TYPE(Socket, CompatibilityFlags::Reader flags) {
    JSG_READONLY_INSTANCE_PROPERTY(readable, getReadable);
    JSG_READONLY_INSTANCE_PROPERTY(writable, getWritable);
    JSG_READONLY_INSTANCE_PROPERTY(closed, getClosed);
    JSG_METHOD(close);
  }

private:
  Socket(jsg::Lock& js, InitData data)
      : readable(kj::mv(data.readable)), writable(kj::mv(data.writable)),
        closeFulfiller(kj::mv(data.closeFulfiller)),
        closedPromise(kj::mv(closeFulfiller->promise)),
        isClosed(false) {
  };

  jsg::Ref<ReadableStream> readable;
  jsg::Ref<WritableStream> writable;
  IoOwn<jsg::PromiseResolverPair<void>> closeFulfiller;
  // This fulfiller is used to resolve the `closedPromise` below.
  jsg::MemoizedIdentity<jsg::Promise<void>> closedPromise;
  bool isClosed;

  kj::Promise<kj::Own<kj::AsyncIoStream>> processConnection();
  void readSideClose();

  void resolveFulfiller(kj::Maybe<kj::Exception> maybeErr) {
    if (isClosed) {
      return;
    }
    auto& context = IoContext::current();
    KJ_IF_MAYBE(err, maybeErr) {
      closeFulfiller->resolver.reject(context.getCurrentLock(), kj::cp(*err));
    } else {
      closeFulfiller->resolver.resolve();
    }
    isClosed = true;
  };

  void errorHandler(jsg::Lock& js, jsg::Value err) {
    auto jsException = err.getHandle(js.v8Isolate);
    auto tunneled = jsg::createTunneledException(js.v8Isolate, jsException);
    resolveFulfiller(jsg::createTunneledException(js.v8Isolate, jsException));
  };

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(readable, writable);
  }
};

jsg::Ref<Socket> connectImplNoOutputLock(
    jsg::Lock& js, jsg::Ref<Fetcher> fetcher, kj::String address);

jsg::Ref<Socket> connectImpl(
    jsg::Lock& js, kj::Maybe<jsg::Ref<Fetcher>> fetcher, kj::String address,
    CompatibilityFlags::Reader featureFlags);

#define EW_SOCKETS_ISOLATE_TYPES         \
  api::Socket,                       \
  api::SocketOptions

// The list of sockets.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE
}  // namespace workerd::api
