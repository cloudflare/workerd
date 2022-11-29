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
  explicit PipelinedAsyncIoStream(kj::Promise<kj::Own<kj::AsyncIoStream>> inner,
      std::function<void()> onClose);
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
  std::function<void()> onClose;
  // Callbacks used to notify the Socket of writable/readable stream closure.
};

struct SocketOptions {
  bool tsl; // TODO(later): TCP socket options need to be implemented.
  JSG_STRUCT(tsl);
};

struct InitData {
  jsg::Ref<ReadableStream> readable;
  jsg::Ref<WritableStream> writable;
  IoOwn<kj::PromiseFulfillerPair<bool>> closeFulfiller;
  IoOwn<kj::PromiseFulfillerPair<void>> jsCloseFulfiller;
  kj::ForkedPromise<void> jsCloseFulfillerFork;
};

class Socket: public jsg::Object {
public:
  Socket(jsg::Lock& js, InitData data)
      : readable(kj::mv(data.readable)), writable(kj::mv(data.writable)),
        closeFulfiller(kj::mv(data.closeFulfiller)),
        jsCloseFulfiller(kj::mv(data.jsCloseFulfiller)),
        jsCloseFulfillerFork(kj::mv(data.jsCloseFulfillerFork)) {
    auto& context = IoContext::current();
    // Attach a callback to close the readable/writable streams when the socket is closed (either
    // explicitly or implicitly).
    context.awaitIo(js, kj::mv(closeFulfiller->promise), [this](
        jsg::Lock& js, bool isImplicit) mutable {
      if (!isImplicit) {
        // A `close` call was made on the socket. Forcibly close the readable/writable streams.
        readable->getController().cancel(js, nullptr).then(js, [writable = kj::mv(writable), this](
            jsg::Lock& js) mutable {
          writable->getController().abort(js, nullptr).then(js,
              [this](jsg::Lock& js) mutable { resolveJsFulfiller(nullptr); },
              [this](jsg::Lock& js, jsg::Value err) { errorHandler(js, kj::mv(err)); });
        }, [this](jsg::Lock& js, jsg::Value err) { errorHandler(js, kj::mv(err)); });
      } else {
        // When the socket is closed implicitly (e.g. when the remote end disconnects), then we
        // cannot cancel its readable/writable streams. So we only resolve the `closed` promise.
        resolveJsFulfiller(nullptr);
      }
    });
  };
  Socket(jsg::Lock& js, kj::Promise<kj::Own<kj::AsyncIoStream>> connectionPromise);

  jsg::Ref<ReadableStream> getReadable() { return readable.addRef(); }
  jsg::Ref<WritableStream> getWritable() { return writable.addRef(); }
  jsg::Promise<void> getClosed() {
    auto& context = IoContext::current();
    return context.awaitIo(jsCloseFulfillerFork.addBranch());
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
  jsg::Ref<ReadableStream> readable;
  jsg::Ref<WritableStream> writable;
  kj::Promise<kj::Own<kj::AsyncIoStream>> processConnection();
  IoOwn<kj::PromiseFulfillerPair<bool>> closeFulfiller;
  // This fulfiller is used to signal either an implicit or explicit socket closure.
  IoOwn<kj::PromiseFulfillerPair<void>> jsCloseFulfiller;
  // Used to signal to JS scripts that socket has been closed. Its `promise` is returned via the
  // `closed` property of the socket.

  kj::ForkedPromise<void> jsCloseFulfillerFork;
  void performClose(bool isImplicit);

  void resolveJsFulfiller(kj::Maybe<kj::Exception> maybeErr) {
    if (!jsCloseFulfiller->fulfiller->isWaiting()) {
      return;
    }
    KJ_IF_MAYBE(err, maybeErr) {
      jsCloseFulfiller->fulfiller->reject(kj::cp(*err));
    } else {
      jsCloseFulfiller->fulfiller->fulfill();
    }
  };

  void errorHandler(jsg::Lock& js, jsg::Value err) {
    auto jsException = err.getHandle(js.v8Isolate);
    auto tunneled = jsg::createTunneledException(js.v8Isolate, jsException);
    resolveJsFulfiller(jsg::createTunneledException(js.v8Isolate, jsException));
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
