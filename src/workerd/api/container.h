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
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

class Fetcher;

class DirectorySnapshot: public jsg::Object {
 public:
  DirectorySnapshot(kj::String id, uint64_t size, kj::String dir, kj::Maybe<kj::String> name)
      : id(kj::mv(id)),
        size(size),
        dir(kj::mv(dir)),
        name(kj::mv(name)) {}

  kj::StringPtr getId() const {
    return id;
  }

  double getSize() const {
    return size;
  }

  kj::StringPtr getDir() const {
    return dir;
  }

  jsg::Optional<kj::StringPtr> getName() const {
    return name.map([](const kj::String& n) -> kj::StringPtr { return n; });
  }

  kj::Maybe<kj::StringPtr> getNameForRpc() const {
    return name.map([](const kj::String& n) -> kj::StringPtr { return n; });
  }

  uint64_t getSizeRaw() const {
    return size;
  }

  JSG_RESOURCE_TYPE(DirectorySnapshot) {
    JSG_READONLY_PROTOTYPE_PROPERTY(id, getId);
    JSG_READONLY_PROTOTYPE_PROPERTY(size, getSize);
    JSG_READONLY_PROTOTYPE_PROPERTY(dir, getDir);
    JSG_READONLY_PROTOTYPE_PROPERTY(name, getName);
  }

  void serialize(jsg::Lock& js, jsg::Serializer& serializer);
  static jsg::Ref<DirectorySnapshot> deserialize(
      jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer);

  JSG_SERIALIZABLE(rpc::SerializationTag::DIRECTORY_SNAPSHOT);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("id", id);
    tracker.trackField("dir", dir);
    tracker.trackField("name", name);
  }

 private:
  kj::String id;
  uint64_t size;
  kj::String dir;
  kj::Maybe<kj::String> name;
};

// Implements the `ctx.container` API for durable-object-attached containers. This API allows
// the DO to supervise the attached container (lightweight virtual machine), including starting,
// stopping, monitoring, making requests to the container, intercepting outgoing network requests,
// etc.
class Container: public jsg::Object {
 public:
  Container(rpc::Container::Client rpcClient, bool running);

  struct SnapshotDirectoryOptions {
    kj::String dir;
    jsg::Optional<kj::String> name;

    JSG_STRUCT(dir, name);
  };

  struct StartupOptions {
    jsg::Optional<kj::Array<kj::String>> entrypoint;
    bool enableInternet = false;
    jsg::Optional<jsg::Dict<kj::String>> env;
    jsg::Optional<int64_t> hardTimeout;
    jsg::Optional<kj::Array<jsg::Ref<DirectorySnapshot>>> snapshots;

    // TODO(containers): Allow intercepting stdin/stdout/stderr by specifying streams here.

    JSG_STRUCT(entrypoint, enableInternet, env, hardTimeout, snapshots);
    JSG_STRUCT_TS_OVERRIDE_DYNAMIC(CompatibilityFlags::Reader flags) {
      if (flags.getWorkerdExperimental()) {
        JSG_TS_OVERRIDE(ContainerStartupOptions {
          entrypoint?: string[];
          enableInternet: boolean;
          env?: Record<string, string>;
          hardTimeout?: number | bigint;
          snapshots?: DirectorySnapshot[];
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
  jsg::Promise<jsg::Ref<DirectorySnapshot>> snapshotDirectory(
      jsg::Lock& js, SnapshotDirectoryOptions options);

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
  api::DirectorySnapshot, api::Container, api::Container::SnapshotDirectoryOptions,                \
      api::Container::StartupOptions

}  // namespace workerd::api
