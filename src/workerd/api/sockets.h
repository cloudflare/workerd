// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include "http.h"


namespace workerd::api {

class PipelinedAsyncIoStream final : public kj::AsyncIoStream, public kj::Refcounted {
  // A stream that is backed by a promise for an AsyncIoStream. All operations on this stream
  // are deferred until the `inner` promise completes.
public:
  explicit PipelinedAsyncIoStream(kj::Promise<kj::Own<kj::AsyncIoStream>> inner);
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
  template <typename T>
  kj::Promise<T> thenOrRunNow(std::function<kj::Promise<T> (kj::Own<kj::AsyncIoStream> *)> f) {
    // Either calls `then` on the `inner` promise with the `f` callback, or calls `f` on an already
    // stored stream (if the `inner` promise completed).
    KJ_IF_MAYBE(e, error) {
      kj::throwRecoverableException(kj::mv(*e));
    }

    KJ_IF_MAYBE(s, completedInner) {
      return f(s);
    } else {
      return inner.then([this, f](kj::Own<kj::AsyncIoStream> stream) -> kj::Promise<T> {
        completedInner = kj::mv(stream);
        return f(&KJ_ASSERT_NONNULL(completedInner));
      });
    }
  }

  kj::Maybe<kj::Own<kj::AsyncIoStream>> completedInner;
  // Stored io stream once the `inner` promise completes.
  kj::Maybe<kj::Exception> error;
  // Stored error if any of the operations throw.
  kj::Promise<kj::Own<kj::AsyncIoStream>> inner;
  // A promise holding a stream that will be available at some point in the future. Typically once
  // a connection is established.
};

struct SocketOptions {
  bool tsl; // TODO(later): TCP socket options need to be implemented.
  JSG_STRUCT(tsl);
};

struct InitData {
  jsg::Ref<ReadableStream> readable;
  jsg::Ref<WritableStream> writable;
  IoOwn<kj::PromiseFulfillerPair<void>> closeFulfiller;
};

class Socket: public jsg::Object {
public:
  Socket(InitData data)
      : readable(kj::mv(data.readable)), writable(kj::mv(data.writable)),
        closeFulfiller(kj::mv(data.closeFulfiller)) {};
  Socket(kj::Promise<kj::Own<kj::AsyncIoStream>> connectionPromise);

  jsg::Ref<ReadableStream> getReadable() { return readable.addRef(); }
  jsg::Ref<WritableStream> getWritable() { return writable.addRef(); }
  jsg::Promise<void> getClosed() {
    // TODO: Right now this promise won't complete when the remote end closes the socket.
    //       Do we need to wrap ReadableStream/WritableStream to make this work or is there a
    //       better way?
    auto& context = IoContext::current();
    return context.awaitIo(kj::mv(closeFulfiller->promise));
  }

  jsg::Promise<void> close(jsg::Lock& js);
  // Closes the socket connection.

  JSG_RESOURCE_TYPE(Socket, CompatibilityFlags::Reader flags) {
    JSG_READONLY_INSTANCE_PROPERTY(readable, getReadable);
    JSG_READONLY_INSTANCE_PROPERTY(writable, getWritable);
    JSG_READONLY_INSTANCE_PROPERTY(closed, getClosed);
    JSG_METHOD(close);
  }

private:
  jsg::Ref<ReadableStream> readable;
  jsg::Ref<WritableStream> writable;
  kj::Promise<kj::Own<kj::AsyncIoStream>> processConnection();
  IoOwn<kj::PromiseFulfillerPair<void>> closeFulfiller;

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
