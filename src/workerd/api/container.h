// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// APIs that an Actor (Durable Object) uses to access its own state.
//
// See actor.h for APIs used by other Workers to talk to Actors.
//
#include <workerd/io/compatibility-date.h>
#include <workerd/io/container.capnp.h>
#include <workerd/io/io-own.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

class Fetcher;

// Implements the `ctx.container` API for durable-object-attached containers. This API allows
// the DO to supervise the attached container (lightweight virtual machine), including starting,
// stopping, monitoring, making requests to the container, intercepting outgoing network requests,
// etc.
class Container: public jsg::Object {
 public:
  Container(rpc::Container::Client rpcClient, bool running);

  struct StartupOptions {
    jsg::Optional<kj::Array<kj::String>> entrypoint;
    bool enableInternet = false;
    jsg::Optional<jsg::Dict<kj::String>> env;
    jsg::Optional<int64_t> hardTimeout;

    // TODO(containers): Allow intercepting stdin/stdout/stderr by specifying streams here.

    JSG_STRUCT(entrypoint, enableInternet, env, hardTimeout);
    JSG_STRUCT_TS_OVERRIDE_DYNAMIC(CompatibilityFlags::Reader flags) {
      if (flags.getWorkerdExperimental()) {
        JSG_TS_OVERRIDE(ContainerStartupOptions {
          entrypoint?: string[];
          enableInternet: boolean;
          env?: Record<string, string>;
          hardTimeout?: number | bigint;
        });
      } else {
        JSG_TS_OVERRIDE(ContainerStartupOptions {
          entrypoint?: string[];
          enableInternet: boolean;
          env?: Record<string, string>;
        });
      }
    }
  };

  bool getRunning() {
    return running;
  }

  // Methods correspond closely to the RPC interface in `container.capnp`.
  void start(jsg::Lock& js, jsg::Optional<StartupOptions> options);
  jsg::MemoizedIdentity<jsg::Promise<void>>& monitor(jsg::Lock& js);
  jsg::Promise<void> destroy(jsg::Lock& js, jsg::Optional<jsg::Value> error);
  void signal(jsg::Lock& js, int signo);
  jsg::Ref<Fetcher> getTcpPort(jsg::Lock& js, int port);
  jsg::Promise<void> setInactivityTimeout(jsg::Lock& js, int64_t durationMs);
  jsg::Promise<void> interceptOutboundHttp(
      jsg::Lock& js, kj::String addr, jsg::Ref<Fetcher> binding);
  jsg::Promise<void> interceptAllOutboundHttp(jsg::Lock& js, jsg::Ref<Fetcher> binding);

  // TODO(containers): listenTcp()

  JSG_RESOURCE_TYPE(Container, CompatibilityFlags::Reader flags) {
    JSG_READONLY_PROTOTYPE_PROPERTY(running, getRunning);
    JSG_METHOD(start);
    JSG_METHOD(monitor);
    JSG_METHOD(destroy);
    JSG_METHOD(signal);
    JSG_METHOD(getTcpPort);
    JSG_METHOD(setInactivityTimeout);

    if (flags.getWorkerdExperimental()) {
      JSG_METHOD(interceptOutboundHttp);
      JSG_METHOD(interceptAllOutboundHttp);
    }
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("destroyReason", destroyReason);
  }

 private:
  IoOwn<rpc::Container::Client> rpcClient;
  bool running;

  kj::Maybe<jsg::Value> destroyReason;

  // Automatically sets up background monitoring if the container is running and not
  // already being monitored. Called from the constructor (if already running) and
  // from start(). This ensures that if the container exits with an error, the
  // IoContext is aborted even if the user never calls monitor().
  void monitorOnBackgroundIfNeeded();

  // State shared between the Container and its background KJ promise continuations.
  // This is a separate refcounted object because KJ promise continuations run without
  // the isolate lock held, making it unsafe to access JSG objects directly. Capturing
  // a reference to this object (via kj::addRef) is safe from KJ continuations.
  struct MonitorState: public kj::Refcounted {
    // True if the user has explicitly called monitor(). When set, the background
    // monitor will not auto-abort the IoContext on container error, since the user's
    // monitor() call will handle the error instead.
    //
    // These fields are mutable because kj::addRef() returns kj::Own<const T>, but
    // the background KJ continuation needs to write the result fields.
    mutable bool monitoringExplicitly = false;

    // When the background monitor completes, it stores the result here so that a
    // subsequent call to monitor() can return immediately without awaitIo(). This
    // avoids registering a pending event (which would block DO hibernation) when the
    // container has already exited.
    //
    // Set to the exit code on success, or to an exception on failure.
    mutable bool finished = false;
    mutable uint8_t exitCode = 0;
    mutable kj::Maybe<kj::Exception> exception;
  };
  IoOwn<MonitorState> monitorState;

  // The forked KJ promise from the monitor RPC call. Both the background monitor and
  // the JS monitor() method share the same underlying RPC request via addBranch().
  // Wrapped in IoOwn because it is a KJ I/O-layer promise.
  kj::Maybe<IoOwn<kj::ForkedPromise<uint8_t>>> monitorKjPromise;

  // The memoized JS promise returned by monitor(). Allows multiple calls to monitor()
  // to return the same promise.
  kj::Maybe<jsg::MemoizedIdentity<jsg::Promise<void>>> monitorJsPromise;

  // The background monitor promise, held to keep it alive. Uses eagerlyEvaluate()
  // to ensure it runs to completion. Held here (rather than via addTask()) to avoid
  // preventing DO hibernation.
  kj::Maybe<IoOwn<kj::Promise<void>>> backgroundMonitor;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(destroyReason);
    visitor.visit(monitorJsPromise);
  }

  class TcpPortWorkerInterface;
  class TcpPortOutgoingFactory;
};

#define EW_CONTAINER_ISOLATE_TYPES api::Container, api::Container::StartupOptions

}  // namespace workerd::api
