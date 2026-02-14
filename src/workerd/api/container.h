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
  jsg::Promise<void> monitor(jsg::Lock& js);
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

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(destroyReason);
  }

  class TcpPortWorkerInterface;
  class TcpPortOutgoingFactory;
};

#define EW_CONTAINER_ISOLATE_TYPES api::Container, api::Container::StartupOptions

}  // namespace workerd::api
