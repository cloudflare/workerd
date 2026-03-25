// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Container management API for Durable Object-attached containers.
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

  struct DirectorySnapshot {
    kj::String id;
    double size;
    kj::String dir;
    jsg::Optional<kj::String> name;

    JSG_STRUCT(id, size, dir, name);
    JSG_STRUCT_TS_OVERRIDE_DYNAMIC(CompatibilityFlags::Reader flags) {
      if (!flags.getWorkerdExperimental()) {
        JSG_TS_OVERRIDE(type DirectorySnapshot = never);
      }
    }
  };

  struct DirectorySnapshotOptions {
    kj::String dir;
    jsg::Optional<kj::String> name;

    JSG_STRUCT(dir, name);
  };

  struct SnapshotRestoreParams {
    DirectorySnapshot snapshot;
    jsg::Optional<kj::String> mountPoint;

    JSG_STRUCT(snapshot, mountPoint);
    JSG_STRUCT_TS_OVERRIDE_DYNAMIC(CompatibilityFlags::Reader flags) {
      if (!flags.getWorkerdExperimental()) {
        JSG_TS_OVERRIDE(type SnapshotRestoreParams = never);
      }
    }
  };

  struct StartupOptions {
    jsg::Optional<kj::Array<kj::String>> entrypoint;
    bool enableInternet = false;
    jsg::Optional<jsg::Dict<kj::String>> env;
    jsg::Optional<int64_t> hardTimeout;
    jsg::Optional<jsg::Dict<kj::String>> labels;
    jsg::Optional<kj::Array<SnapshotRestoreParams>> snapshots;

    // TODO(containers): Allow intercepting stdin/stdout/stderr by specifying streams here.

    JSG_STRUCT(entrypoint, enableInternet, env, hardTimeout, labels, snapshots);
    JSG_STRUCT_TS_OVERRIDE_DYNAMIC(CompatibilityFlags::Reader flags) {
      if (flags.getWorkerdExperimental()) {
        JSG_TS_OVERRIDE(ContainerStartupOptions {
          entrypoint?: string[];
          enableInternet: boolean;
          env?: Record<string, string>;
          hardTimeout?: number | bigint;
          labels?: Record<string, string>;
          snapshots?: ContainerSnapshotRestoreParams[];
        });
      } else {
        JSG_TS_OVERRIDE(ContainerStartupOptions {
          entrypoint?: string[];
          enableInternet: boolean;
          env?: Record<string, string>;
          hardTimeout?: never;
          labels?: Record<string, string>;
          snapshots?: never;
        });
      }
    }
  };

  bool getRunning() const {
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
  jsg::Promise<void> interceptOutboundHttps(
      jsg::Lock& js, kj::String addr, jsg::Ref<Fetcher> binding);
  jsg::Promise<DirectorySnapshot> snapshotDirectory(
      jsg::Lock& js, DirectorySnapshotOptions options);

  // TODO(containers): listenTcp()

  JSG_RESOURCE_TYPE(Container, CompatibilityFlags::Reader flags) {
    JSG_READONLY_PROTOTYPE_PROPERTY(running, getRunning);
    JSG_METHOD(start);
    JSG_METHOD(monitor);
    JSG_METHOD(destroy);
    JSG_METHOD(signal);
    JSG_METHOD(getTcpPort);
    JSG_METHOD(setInactivityTimeout);

    JSG_METHOD(interceptOutboundHttp);
    JSG_METHOD(interceptAllOutboundHttp);
    if (flags.getWorkerdExperimental()) {
      JSG_METHOD(interceptOutboundHttps);
      JSG_METHOD(snapshotDirectory);
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

#define EW_CONTAINER_ISOLATE_TYPES                                                                 \
  api::Container, api::Container::DirectorySnapshot, api::Container::DirectorySnapshotOptions,     \
      api::Container::SnapshotRestoreParams, api::Container::StartupOptions

}  // namespace workerd::api
