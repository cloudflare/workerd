// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The private state behind worker-factory.h, shared by the factory's source files.

#include "worker-factory.h"

#include <workerd/api/bench.h>
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

// A Worker of the config that the loops of a process share (`--loops`): compiled once, by the
// first loop to need it, and served by every loop through a `CompiledWorker` of its own, which
// holds a reference to this.
//
// It has what is the same for every loop and outlives none of them: the `Worker`, with its
// script and isolate, which any thread may use under the isolate lock, and what the isolate
// points into. What a request needs of its own loop (the timer, the task set, the channels)
// is in the loop's `CompiledWorker::Impl`. Immutable once constructed. The loop that drops
// the last reference destroys it, and with it the isolate.
class SharedWorker final: public kj::AtomicRefcounted {
 public:
  SharedWorker(const SharedFactory::Impl& factory,
      uint64_t key,
      kj::Own<capnp::MessageReader> config,
      kj::Own<capnp::MallocMessageBuilder> flagsArena,
      kj::Own<const Worker> worker,
      const CompiledWorker::Impl& compiled);
  // Takes itself out of the factory's table.
  ~SharedWorker() noexcept(false);

  // The factory outlives every loop's factory, and so every reference to this.
  const SharedFactory::Impl& factory;
  const uint64_t key;
  // The reader of the config message the isolate was compiled from, which its `WorkerdApi`
  // reads the extensions through: not a loop's own reader, which goes with that loop.
  kj::Own<capnp::MessageReader> config;
  kj::Own<capnp::MallocMessageBuilder> flagsArena;
  kj::Own<const Worker> worker;
  kj::Maybe<kj::String> accessBlobHeader;
  WorkerInfo info;
  api::WorkerStartupTiming startupTiming;
};

// The shared Workers of a process, by `SharedWorker::key`.
struct SharedWorkers {
  struct Slot {
    // The worker, once a loop has compiled it. Not a reference: the entry is removed by the
    // worker's destructor, so a lookup that races with it takes a weak reference.
    kj::Maybe<const SharedWorker&> worker;
    // A loop is compiling the worker; the others wait for it.
    bool compiling = false;
    kj::Vector<kj::Own<kj::CrossThreadPromiseFulfiller<void>>> waiters;
  };
  kj::HashMap<uint64_t, Slot> slots;

  static uint64_t key(uint32_t configService, uint32_t copy) {
    return (static_cast<uint64_t>(configService) << 32) | copy;
  }
};

class SharedFactory::Impl final {
 public:
  Impl(jsg::V8System& v8System,
      kj::EntropySource& entropySource,
      kj::Filesystem& fs,
      kj::Own<Options> options,
      ::rust::Vec<uint64_t> config);

  jsg::V8System& v8System;
  kj::EntropySource& entropySource;
  kj::Filesystem& fs;
  // The Pyodide bundle manager in `pythonConfig` is const too: it guards what it loads.
  kj::Own<const Options> options;

  // The config message. Each loop's factory reads it through a reader of its own, since a
  // reader counts what it traverses.
  ::rust::Vec<uint64_t> configMessage;

  // The one header table of the run, and the IDs in it of the headers each loop's capnp factory
  // and `ThreadContext` use, so that the loops agree on them. The builder is only used while
  // constructing.
  kj::HttpHeaderTable::Builder headerTableBuilder;
  capnp::HttpOverCapnpFactory::HeaderIdBundle httpOverCapnpHeaderIds;
  ThreadContext::HeaderIdBundle threadContextHeaderIds;
  kj::Own<kj::HttpHeaderTable> headerTable;

  // Isolates take the provider by mutable reference; all it offers is thread-safe.
  mutable kj::Own<api::MemoryCacheProvider> memoryCacheProvider;
  // Every loop's `ChannelTokenHandler` has a copy, so each decodes the others' tokens.
  ChannelTokenHandler::Key channelTokenKey;
  // Set when the primary loop starts the inspector.
  kj::MutexGuarded<kj::Maybe<kj::Own<InspectorServiceIsolateRegistrar>>> inspectorRegistrar;
  // The config's Workers that loops share. Empty unless a loop's factory shares
  // (`factory_share_workers`), and again once those loops are gone.
  kj::MutexGuarded<SharedWorkers> sharedWorkers;
};

// Writes a `Frankenvalue` for another loop thread of the process: the capabilities of its cap
// table cross as channel tokens, which each loop resolves in its own graph.
class LoopFrankenvalueHandler final: public FrankenvalueHandler {
 public:
  explicit LoopFrankenvalueHandler(ChannelTokenHandler& tokens): tokens(tokens) {}
  void toCapnp(Frankenvalue& value, rpc::Frankenvalue::Builder builder) override {
    tokens.frankenvalueToCapnp(value, builder);
  }

 private:
  ChannelTokenHandler& tokens;
};

class WorkerFactory::Impl final: private kj::TaskSet::ErrorHandler {
 public:
  Impl(WorkerFactory& factory,
      kj::Arc<SharedFactory> shared,
      kj::Timer& timer,
      const kj::MonotonicClock& monotonicClock,
      kj::Network& network,
      PrimaryLoop primary);

  // Declared first, so that everything else here is destroyed before the process-wide state
  // (V8 among it) this may hold the last reference to.
  kj::Arc<SharedFactory> sharedRef;
  const SharedFactory::Impl& shared;
  PrimaryLoop primary;
  // Set by `factory_share_workers`: this loop serves the config's Workers from the copies of
  // this number that the process's loops share, instead of compiling its own.
  kj::Maybe<uint32_t> sharedCopy;
  // The loop this factory is bound to.
  const kj::Executor& executor;

  kj::Timer& timer;
  const kj::MonotonicClock& monotonicClock;
  kj::Network& network;
  // Expires with this factory, which owns every container cleanup that uses `network`.
  WeakOwner<kj::Network&> weakNetwork;

  // This loop's reader of the shared config message.
  capnp::FlatArrayMessageReader configReader;
  config::Config::Reader config;

  capnp::ByteStreamFactory byteStreamFactory;
  capnp::HttpOverCapnpFactory httpOverCapnpFactory;
  ThreadContext threadContext;

  ChannelTokenHandler channelTokenHandler;
  // Writes the Frankenvalues this loop sends another loop of the process.
  LoopFrankenvalueHandler loopFrankenvalueHandler;
  kj::Maybe<::rust::Box<ServerHandle>> server;

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
  // Throws unless this is the primary loop's factory; `what` names the operation.
  void requirePrimary(kj::StringPtr what) const;

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

// Everything the requests of one loop on a compiled worker share. Refcounted so that the I/O
// channel factory of a request in flight keeps it alive after the server drops the worker.
//
// Bound to its loop. For a Worker that loops share, this is the loop's view of it: `shared` is
// set, `worker` is another reference to the `SharedWorker`'s, and `name`, `accessBlobHeader`,
// `info` and `startupTiming` are copies.
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
  // The Worker as the loops share it, if they do; it owns what `worker` points into.
  kj::Maybe<kj::Own<const SharedWorker>> shared;
  // Held from compilation until `worker_set_ctx_exports()` fills the object in; none for a worker
  // whose script failed to compile, and for a view of a Worker another loop compiled.
  kj::Maybe<jsg::V8Ref<v8::Object>> ctxExportsHandle;

  WorkerInfo info;
  // How long the phases of compiling the worker took, which `workerd bench` reports for the
  // throwaway copies `factory_bench_startup()` compiles.
  api::WorkerStartupTiming startupTiming;

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

  // Declared first: the VFS's hooks and the alarm scheduler call into it.
  ::rust::Box<ActorNamespaceHandle> actors;
  kj::Maybe<kj::Own<const kj::Directory>> directory;
  kj::Own<const kj::Directory> vfsDirectory;  // the in-memory directory when there is none
  // What SQLite opens files through: `vfsDirectory`, or in a cluster a wrapper that fences opens
  // on the lease and reports WAL writes, which the cluster ignores for the scheduler's database.
  kj::Own<const kj::Directory> sqliteDirectory;
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
  // A connection and its RPC system, shared by the capabilities obtained over it.
  struct Session: public kj::Refcounted {
    Session(kj::Own<kj::AsyncIoStream> connection, FrankenvalueHandler& frankenvalueHandler)
        : connection(kj::mv(connection)),
          rpcSystem(*this->connection),
          frankenvalueHandler(frankenvalueHandler) {}
    kj::Own<kj::AsyncIoStream> connection;
    capnp::TwoPartyClient rpcSystem;
    // Writes the Frankenvalues sent over the session; it outlives the factory's sessions.
    FrankenvalueHandler& frankenvalueHandler;
  };

  Impl(const WorkerFactory& factory, kj::Own<Session> session, rpc::WorkerdBootstrap::Client target)
      : factory(factory),
        session(kj::mv(session)),
        target(kj::mv(target)) {}

  const WorkerFactory& factory;
  kj::Own<Session> session;
  // Where events go: the session's bootstrap, or an actor obtained over it.
  rpc::WorkerdBootstrap::Client target;
};

// The `WorkerdBootstrap` a peer sees over capnp: every event it starts goes to `service`.
//
// A peer that is another loop thread of this process may also restore a stub whose source is
// `service`: its `restore()` events carry their parameters' capabilities as channel tokens
// (`LoopFrankenvalueHandler`), which `restoreTokens` resolves in this loop's graph.
class WorkerdBootstrapImpl final: public rpc::WorkerdBootstrap::Server {
 public:
  WorkerdBootstrapImpl(kj::Rc<IoChannelFactory::SubrequestChannel> service,
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      kj::Maybe<ChannelTokenHandler&> restoreTokens = kj::none);
  kj::Promise<void> startEvent(StartEventContext context) override;

 private:
  kj::Rc<IoChannelFactory::SubrequestChannel> service;
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  kj::Maybe<ChannelTokenHandler&> restoreTokens;
  class EventDispatcherImpl;
};

// The debug port: every service's entrypoints and actors, looked up through the server. It is
// also what a cluster peer and another loop thread of the process (`localCaller`) see; the
// latter has `restoreTokens`, so the actors and entrypoints it gets take its `restore()` events.
class WorkerdDebugPortImpl final: public rpc::WorkerdDebugPort::Server {
 public:
  WorkerdDebugPortImpl(::rust::Box<ServerHandle> server,
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      LocalCaller localCaller,
      kj::Maybe<ChannelTokenHandler&> restoreTokens = kj::none);
  kj::Promise<void> getEntrypoint(GetEntrypointContext context) override;
  kj::Promise<void> getActor(GetActorContext context) override;

 private:
  ::rust::Box<ServerHandle> server;
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  LocalCaller localCaller;
  kj::Maybe<ChannelTokenHandler&> restoreTokens;
};

// Trims the name off a durable id longer than production keeps (1024 bytes).
Worker::Actor::Id normalizeActorId(Worker::Actor::Id id);

}  // namespace workerd::server
