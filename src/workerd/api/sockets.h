// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include "http.h"


namespace workerd::api {

struct SocketOptions {
  jsg::Unimplemented tls; // TODO(later): TCP socket options need to be implemented.
  JSG_STRUCT(tls);
};

class Socket: public jsg::Object {
public:
  Socket(jsg::Lock& js, jsg::Ref<ReadableStream> readable, jsg::Ref<WritableStream> writable,
      kj::Own<jsg::PromiseResolverPair<void>> close)
      : readable(kj::mv(readable)), writable(kj::mv(writable)),
        closeFulfiller(kj::mv(close)),
        closedPromise(kj::mv(closeFulfiller->promise)),
        isClosed(false) {
  };

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
  jsg::Ref<ReadableStream> readable;
  jsg::Ref<WritableStream> writable;
  kj::Own<jsg::PromiseResolverPair<void>> closeFulfiller;
  // This fulfiller is used to resolve the `closedPromise` below.
  jsg::MemoizedIdentity<jsg::Promise<void>> closedPromise;
  bool isClosed;

  kj::Promise<kj::Own<kj::AsyncIoStream>> processConnection();

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
