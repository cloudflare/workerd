// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The private state behind worker-factory.h, shared by the factory's source files.

#include "worker-factory.h"

#include <workerd/io/actor-sqlite.h>
#include <workerd/io/io-thread-context.h>
#include <workerd/server/alarm-scheduler.h>
#include <workerd/server/container-client.h>
#include <workerd/server/facet-tree-index.h>
#include <workerd/server/server/bridge.rs.h>

#include <capnp/rpc-twoparty.h>
#include <capnp/serialize.h>

namespace workerd::server {

using kj_rs::Rust;

// A kj string as Rust text. A kj string need not be UTF-8 (a JS string with a lone surrogate, a
// peer's bytes): `toRust` throws for one that is not, as suits a name or a key; `toRustString`
// replaces what is not with U+FFFD, as suits free text (an exception's description), and does
// not throw.
inline ::rust::Str toRust(kj::StringPtr str) {
  return ::rust::Str(str.begin(), str.size());
}
inline ::rust::String toRustString(kj::StringPtr str) {
  return ::rust::String::lossy(str.begin(), str.size());
}
inline kj::Maybe<::rust::Str> toRust(kj::Maybe<kj::StringPtr> str) {
  return str.map([](kj::StringPtr s) { return toRust(s); });
}
inline kj::Maybe<kj::String> toKj(const kj::Maybe<::rust::Str>& str) {
  return str.map([](const ::rust::Str& s) { return kj::str(s); });
}
inline kj::Maybe<kj::String> toKj(const kj::Maybe<::rust::String>& str) {
  return str.map([](const ::rust::String& s) { return kj::str(s); });
}
inline kj::Own<Frankenvalue> toOwn(Frankenvalue value) {
  return kj::heap<Frankenvalue>(kj::mv(value));
}
inline kj::Maybe<kj::Own<Frankenvalue>> toOwn(kj::Maybe<Frankenvalue> value) {
  return value.map([](Frankenvalue& v) { return toOwn(kj::mv(v)); });
}

// =======================================================================================

class InspectorService;

// The registration point for isolates with the inspector service, which lives on its own thread.
// The inspector service attaches itself when it starts and detaches when it stops.
class InspectorServiceIsolateRegistrar final {
 public:
  InspectorServiceIsolateRegistrar() = default;
  ~InspectorServiceIsolateRegistrar() noexcept(true);
  KJ_DISALLOW_COPY_AND_MOVE(InspectorServiceIsolateRegistrar);

  void registerIsolate(kj::StringPtr name, Worker::Isolate& isolate);

 private:
  void attach(InspectorService& service);
  void detach();

  kj::MutexGuarded<kj::Maybe<InspectorService&>> inspectorService;
  friend class InspectorService;
};

// Starts the inspector on `address` on its own thread and returns the port it listens on.
uint startInspector(kj::String address, InspectorServiceIsolateRegistrar& registrar);

// =======================================================================================

class WorkerFactory::Impl final: private kj::TaskSet::ErrorHandler {
 public:
  Impl(WorkerFactory& factory,
      jsg::V8System& v8System,
      kj::Timer& timer,
      const kj::MonotonicClock& monotonicClock,
      kj::Network& network,
      kj::EntropySource& entropySource,
      kj::Filesystem& fs,
      kj::Own<Options> options,
      ::rust::Vec<uint64_t> config);

  jsg::V8System& v8System;
  kj::Timer& timer;
  const kj::MonotonicClock& monotonicClock;
  kj::Network& network;
  kj::EntropySource& entropySource;
  kj::Filesystem& fs;
  kj::Own<Options> options;

  // The reader borrows the message, so the message is declared first.
  ::rust::Vec<uint64_t> configMessage;
  capnp::FlatArrayMessageReader configReader;
  config::Config::Reader config;

  capnp::ByteStreamFactory byteStreamFactory;
  kj::HttpHeaderTable::Builder headerTableBuilder;
  capnp::HttpOverCapnpFactory httpOverCapnpFactory;
  ThreadContext threadContext;
  kj::Own<kj::HttpHeaderTable> headerTable;

  kj::Own<api::MemoryCacheProvider> memoryCacheProvider;
  ChannelTokenHandler channelTokenHandler;
  kj::Maybe<::rust::Box<ServerHandle>> server;
  kj::Maybe<kj::Own<InspectorServiceIsolateRegistrar>> inspectorRegistrar;

  // The Docker client of an actor's container. Actors and `setInactivityTimeout()` timers own the
  // client; while one lives, the actor's next incarnation takes the same one.
  struct Container {
    kj::Maybe<ContainerClient&> client;
    // Cancels `cleanup` when the next client takes the container over.
    kj::Canceler canceler;
    // Docker removing the container, once the last client to hold it has shut down.
    kj::ForkedPromise<void> cleanup = kj::Promise<void>(kj::READY_NOW).fork();
  };
  // By container id. A callback of each client points at its entry.
  kj::HashMap<kj::String, kj::Own<Container>> containers;
  // Set by factory_shutdown_containers: no actor may acquire a container client any more.
  bool containerShutdownStarted = false;

  // Background work no request owns: container cleanup, trace delivery, and the Rust server's
  // tasks (factory_spawn). Declared last, so that it is destroyed first: a task may hold anything
  // else here.
  kj::TaskSet tasks;
  // How many tasks factory_spawn has added; factory_settle_tasks waits for it to stop growing.
  uint64_t spawnCount = 0;

  const ServerHandle& getServer() const;

 private:
  void taskFailed(kj::Exception&& exception) override;
};

// The clock JavaScript sees: the calendar clock for `now()`, timers measured from a fresh
// monotonic reading so that time spent in JavaScript since the last poll does not shorten them.
class WorkerTimerChannel final: public TimerChannel {
 public:
  WorkerTimerChannel(kj::Timer& timer, const kj::MonotonicClock& monotonicClock)
      : timer(timer),
        monotonicClock(monotonicClock) {}

  void syncTime() override {}
  kj::Date now(kj::Maybe<kj::Date>) override {
    return kj::systemPreciseCalendarClock().now();
  }
  kj::Promise<void> atTime(kj::Date when) override {
    return timer.atTime(monotonicClock.now() + (when - now(kj::none)));
  }
  kj::Promise<void> afterLimitTimeout(kj::Duration t) override {
    return timer.afterDelay(t);
  }
  kj::TimePoint nowForLimitTimeout() override {
    return monotonicClock.now();
  }

 private:
  kj::Timer& timer;
  const kj::MonotonicClock& monotonicClock;
};

// Everything requests on a compiled worker share. Refcounted so that the I/O channel factory of a
// request in flight keeps it alive after the server drops the worker.
class CompiledWorker::Impl final: public kj::Refcounted, private kj::TaskSet::ErrorHandler {
 public:
  Impl(const WorkerFactory& factory, kj::String name);

  const WorkerFactory& factory;
  kj::String name;
  WorkerTimerChannel timerChannel;
  kj::TaskSet waitUntilTasks;

  // The compiled compatibility flags of a config worker; a dynamic worker's live in its source.
  kj::Own<capnp::MallocMessageBuilder> flagsArena;
  // A dynamic worker's source: its `env`, its tails and the content its script points into.
  kj::Maybe<DynamicWorkerSource> dynamicSource;
  // A dynamic worker's I/O channel tables, numbered as the runtime numbers channels: the
  // subrequest table starts with `IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT` slots for the
  // global outbound (or a null outbound when the source gave none), then the capabilities the
  // `env` held in cap-table order; actor classes and RPC channels start from zero. The Rust
  // channel factory serves the numbers past these tables (the worker's own `ctx.exports`). Empty
  // for a config worker, whose whole table is the Rust side's.
  kj::Vector<kj::Rc<IoChannelFactory::SubrequestChannel>> subrequestChannels;
  kj::Vector<kj::Rc<IoChannelFactory::ActorClassChannel>> actorClassChannels;
  kj::Vector<kj::Rc<IoChannelFactory::RpcChannel>> rpcChannels;
  kj::Maybe<kj::String> accessBlobHeader;
  bool isDynamic = false;

  kj::Maybe<kj::Own<const Worker>> worker;
  // Held from compilation until `worker_set_ctx_exports()` fills the object in; none for a worker
  // whose script failed to compile.
  kj::Maybe<jsg::V8Ref<v8::Object>> ctxExportsHandle;

  WorkerInfo info;

  const Worker& getWorker() const {
    return *KJ_ASSERT_NONNULL(worker);
  }

 private:
  void taskFailed(kj::Exception&& exception) override;
};

// The storage of a namespace: its directory (none in memory), the VFS, and the alarm scheduler.
class ActorStorage::Impl final: public kj::Refcounted {
 public:
  Impl(const WorkerFactory& factory,
      kj::Maybe<kj::Own<const kj::Directory>> directory,
      kj::String uniqueKey,
      ::rust::Box<ActorNamespaceHandle> actors);

  kj::Maybe<kj::Own<const kj::Directory>> directory;
  kj::Own<const kj::Directory> vfsDirectory;  // the in-memory directory when there is none
  SqliteDatabase::Vfs vfs;
  kj::String uniqueKey;
  kj::Own<AlarmScheduler> alarmScheduler;

  // The facet index of the root actor `rootKey`, read from disk for one operation; none when it
  // does not exist (so no facet can have storage).
  kj::Maybe<kj::Own<FacetTreeIndex>> getFacetTreeIndexIfNotEmpty(
      const kj::Directory& dir, kj::StringPtr rootKey);

  kj::Path getSqlitePath(kj::StringPtr rootKey, uint facetId, kj::StringPtr suffix = ""_kj);
  void deleteFacet(
      const kj::Directory& dir, kj::StringPtr rootKey, FacetTreeIndex& index, uint facetId);
  void deleteDescendantStorage(const kj::Directory& dir, kj::StringPtr rootKey, uint parentId);
  void cloneFacet(const kj::Directory& dir,
      kj::StringPtr rootKey,
      FacetTreeIndex& index,
      uint srcId,
      uint dstId);
};

class RpcClient::Impl final {
 public:
  Impl(const WorkerFactory& factory, kj::Own<kj::AsyncIoStream> connection)
      : factory(factory),
        connection(kj::mv(connection)),
        rpcSystem(*this->connection) {}

  const WorkerFactory& factory;
  kj::Own<kj::AsyncIoStream> connection;
  capnp::TwoPartyClient rpcSystem;
};

// The `WorkerdBootstrap` a peer sees over capnp: every event it starts goes to `service`.
class WorkerdBootstrapImpl final: public rpc::WorkerdBootstrap::Server {
 public:
  WorkerdBootstrapImpl(kj::Rc<IoChannelFactory::SubrequestChannel> service,
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory);
  kj::Promise<void> startEvent(StartEventContext context) override;

 private:
  kj::Rc<IoChannelFactory::SubrequestChannel> service;
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  class EventDispatcherImpl;
};

// The debug port: every service's entrypoints and actors, looked up through the server.
class WorkerdDebugPortImpl final: public rpc::WorkerdDebugPort::Server {
 public:
  WorkerdDebugPortImpl(
      ::rust::Box<ServerHandle> server, capnp::HttpOverCapnpFactory& httpOverCapnpFactory);
  kj::Promise<void> getEntrypoint(GetEntrypointContext context) override;
  kj::Promise<void> getActor(GetActorContext context) override;

 private:
  ::rust::Box<ServerHandle> server;
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
};

// Trims the name off a durable id longer than production keeps (1024 bytes).
Worker::Actor::Id normalizeActorId(Worker::Actor::Id id);

}  // namespace workerd::server
