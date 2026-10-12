// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The worker factory: the part of workerd's server that needs the isolate, kept in C++ and driven
// by the Rust server through the cxx bridge in server/bridge.rs. It compiles workers, starts
// requests on them, constructs actors and their storage, speaks the capnp RPC protocols, encodes
// channel tokens and runs the inspector. Policy that is workerd's alone (the unlimited
// LimitEnforcer, the clock-corrected TimerChannel, the null IsolateLimitEnforcer, the default
// observers) lives here too: the production runtime supplies its own.
//
// Every function the bridge declares is a free function in this namespace taking the object as its
// first argument, and is documented on its declaration there. One that can throw is declared
// fallible in the bridge, where a C++ exception becomes a `Result` error on the Rust side; the rest
// do not throw.

#include <workerd/api/memory-cache.h>
#include <workerd/api/pyodide/pyodide.h>
#include <workerd/io/io-channels.h>
#include <workerd/io/request-tracker.h>
#include <workerd/io/worker.h>
#include <workerd/rust/kj/ffi.h>
#include <workerd/rust/worker/bridge.h>
#include <workerd/server/channel-token.h>
#include <workerd/server/workerd.capnp.h>

#include <kj-rs/kj-rs.h>

#include <capnp/compat/byte-stream.h>
#include <capnp/compat/http-over-capnp.h>
#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/filesystem.h>

namespace workerd::jsg {
class V8System;
}

// The bridge names kj's handler as kj-hyper's bridge does (kj-hyper/kj-hyper.h).
namespace workerd::rust::kj_hyper {
using WebSocketErrorHandler = kj::WebSocketErrorHandler;
}

namespace workerd::server {

// Rust types of the bridge, defined in server/channels.rs and server/tasks.rs.
struct ServerHandle;
struct ChannelFactory;
struct SubrequestChannel;
struct SpawnedTask;
struct ActorClassChannel;
struct ActorNamespaceHandle;
struct ActorHooks;
struct AbortReason;
struct KeepAlive;
struct WorkerInterfaceList;
struct WorkerStub;
struct UdpFlow;

// The hibernation manager of an evicted actor, handed to its replacement.
using HibernationManager = Worker::Actor::HibernationManager;
// The isolate lock, as an eviction takes it to hibernate the actor's WebSockets.
using ActorLock = Worker::AsyncLock;
// A `Worker::Actor::Id`: a 32-byte durable id, or an ephemeral name.
using ActorIdHandle = Worker::Actor::Id;
// The source of a dynamic worker, as `IoChannelFactory::loadIsolate()` receives it: the function
// that fetches it. `factory_new_worker()` fetches the source and compiles the worker from it; the
// capabilities in its `env` become the worker's own channel tables (`CompiledWorker::Impl`).
using DynamicSource = kj::Function<kj::Promise<DynamicWorkerSource>()>;
// One request's metadata.
using RequestMetadata = IoChannelFactory::SubrequestMetadata;
// What a channel token is for: RPC, or Durable Object storage.
using TokenUsage = IoChannelFactory::ChannelTokenUsage;
// A channel token as the ChannelTokenHandler encodes it: ready bytes, or a promise of them.
using PendingToken = kj::OneOf<kj::Array<kj::byte>, kj::Promise<kj::Array<kj::byte>>>;
// A facet's start info, as `Worker::Actor::FacetManager::getFacet()` receives it: the function
// that resolves the facet's class and id.
using FacetStart = kj::Function<kj::Promise<Worker::Actor::FacetManager::StartInfo>()>;
// An actor's `onBroken()` promise. It holds no reference to the actor: it rejects when the actor
// is destroyed.
using ActorBroken = kj::Promise<void>;

class CompiledWorker;
class ActorHandle;
class ActorStorage;
class SubrequestChannelHandle;
class ActorClassChannelHandle;
class RpcClient;
struct WorkerSpec;
struct WorkerInfo;
struct ContainerRef;
struct FacetStartInfo;
struct ActorStorageSpec;
struct HeaderEdit;
struct UdpDatagram;

// How the config message is read: configs can legitimately be very large and are not malicious.
constexpr capnp::ReaderOptions CONFIG_READER_OPTIONS = {.traversalLimitInWords = kj::maxValue};
inline kj::ArrayPtr<const capnp::word> asWords(kj::ArrayPtr<const uint64_t> words) {
  return kj::arrayPtr(reinterpret_cast<const capnp::word*>(words.begin()), words.size());
}

// =======================================================================================

// Everything the workers of one run share.
class WorkerFactory final: private ChannelTokenHandler::Resolver {
 public:
  struct Options {
    bool experimental = false;
    // When set, every worker uses this compatibility date and none may specify its own.
    kj::Maybe<kj::String> testCompatibilityDateOverride;
    Worker::LoggingOptions loggingOptions;
    api::pyodide::PythonConfig pythonConfig;
  };

  // `config` is the encoded config message; the factory owns it for the run and reads it with an
  // unlimited traversal limit, as the server does. `options` is owned rather than moved because
  // `PythonConfig` cannot be moved.
  // `monotonicClock` must read consistently with `timer` whenever the timer is advanced.
  WorkerFactory(jsg::V8System& v8System,
      kj::Timer& timer,
      const kj::MonotonicClock& monotonicClock,
      kj::Network& network,
      kj::EntropySource& entropySource,
      kj::Filesystem& fs,
      kj::Own<Options> options,
      ::rust::Vec<uint64_t> config);
  ~WorkerFactory() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(WorkerFactory);

  class Impl;
  Impl& getImpl() const;

 private:
  // Bridge functions receive every handle as `const&`, so the state behind one is `mutable`.
  mutable kj::Own<Impl> impl;

  kj::Rc<IoChannelFactory::SubrequestChannel> resolveEntrypoint(kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue props,
      Persistent persistent) override;
  kj::Rc<IoChannelFactory::ActorClassChannel> resolveActorClass(kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue props,
      Persistent persistent) override;
  kj::Rc<IoChannelFactory::ActorChannel> resolveActor(kj::StringPtr namespaceKey,
      kj::ArrayPtr<const byte> id,
      kj::Maybe<kj::StringPtr> name,
      Persistent persistent) override;
};

::rust::Slice<const uint8_t> factory_config(const WorkerFactory& factory);
const kj::HttpHeaderTable& factory_header_table(const WorkerFactory& factory);
bool factory_experimental(const WorkerFactory& factory);
void factory_set_server(const WorkerFactory& factory, ::rust::Box<ServerHandle> server);
void factory_spawn(const WorkerFactory& factory, ::rust::Box<SpawnedTask> task);
void factory_clear_tasks(const WorkerFactory& factory);
kj::Promise<void> factory_settle_tasks(const WorkerFactory& factory);
uint64_t factory_timer_now(const WorkerFactory& factory);
kj::Promise<void> factory_sleep(const WorkerFactory& factory, uint64_t nanos);

kj::Promise<kj::Own<CompiledWorker>> factory_new_worker(const WorkerFactory& factory,
    const WorkerSpec& spec,
    kj::Maybe<uint32_t> configService,
    kj::Maybe<kj::Own<DynamicSource>> dynamicSource);

// =======================================================================================

// A compiled worker: its isolate, script and `Worker`, and what compiling them revealed.
class CompiledWorker final {
 public:
  class Impl;
  explicit CompiledWorker(kj::Own<Impl> impl);
  ~CompiledWorker() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(CompiledWorker);

  Impl& getImpl() const;

 private:
  mutable kj::Own<Impl> impl;
};

WorkerInfo worker_info(const CompiledWorker& worker);
void worker_set_ctx_exports(const CompiledWorker& worker, ::rust::Slice<const uint64_t> globals);
void worker_unlink(const CompiledWorker& worker);
kj::Own<WorkerInterface> worker_start_request(const CompiledWorker& worker,
    kj::Maybe<::rust::Str> entrypoint,
    kj::Maybe<kj::Own<Frankenvalue>> props,
    kj::Maybe<const ActorHandle&> actor,
    ::rust::Box<ChannelFactory> channels,
    kj::Own<RequestMetadata> metadata,
    ::rust::Box<WorkerInterfaceList> tails);
kj::Own<WorkerInterface> worker_interface_attach(
    kj::Own<WorkerInterface> inner, ::rust::Box<KeepAlive> keep);
kj::Own<ActorHandle> worker_new_actor(const CompiledWorker& worker,
    ::rust::Str className,
    kj::Maybe<kj::Own<Frankenvalue>> props,
    kj::Own<ActorIdHandle> id,
    const ActorStorage& storage,
    const ActorStorageSpec& spec,
    ::rust::Box<ActorHooks> hooks,
    kj::Maybe<kj::Rc<HibernationManager>> hibernationManager,
    kj::Maybe<const ContainerRef&> container);

// =======================================================================================

// A live actor. Owns the `Worker::Actor`, the hooks it borrows (its facet manager and its request
// tracker's hooks, one object, which the actor also holds as its loopback) and its request
// tracker; requests on it are started through the worker. Every actor reference `addRef()` hands
// out counts as an active request until it is dropped, and the tracker's hooks tell the server
// when the actor goes from idle to active and back. The handle's own reference is not counted, so
// `Worker::Actor::isShared()` is "a request holds the actor".
class ActorHandle final {
 public:
  ActorHandle(kj::Own<Worker::Actor::FacetManager> hooks,
      kj::Own<RequestTracker> tracker,
      kj::Own<Worker::Actor> actor);
  // Silences the tracker's hooks: references handed out earlier may outlive the handle.
  ~ActorHandle() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ActorHandle);
  Worker::Actor& getActor() const;
  kj::Own<Worker::Actor> addRef() const;

 private:
  kj::Own<Worker::Actor::FacetManager> hooks;
  kj::Own<RequestTracker> tracker;
  mutable kj::Own<Worker::Actor> actor;
};

kj::Own<ActorBroken> actor_on_broken(const ActorHandle& actor);
kj::Promise<void> actor_broken(kj::Own<ActorBroken> broken);
void actor_abort(const ActorHandle& actor, const AbortReason& reason);
kj::Promise<kj::Own<ActorLock>> actor_lock(const ActorHandle& actor);
bool actor_shutdown(
    const ActorHandle& actor, ::rust::Str reason, kj::Maybe<kj::Own<ActorLock>> lock);
void actor_reset_storage(const ActorHandle& actor);
kj::Maybe<kj::Rc<HibernationManager>> actor_hibernation_manager(const ActorHandle& actor);
kj::Promise<FacetStartInfo> facet_start_resolve(kj::Own<FacetStart> start);

kj::Own<ActorIdHandle> actor_id_clone(const ActorIdHandle& id);
::rust::String actor_id_key(const ActorIdHandle& id);
kj::Own<ActorIdHandle> actor_id_from_name(::rust::Str name);
kj::Own<ActorIdHandle> actor_id_from_hex(::rust::Str hex);

// The storage of one Durable Object namespace: its directory (or none, for in-memory storage),
// the SQLite VFS over it, and the alarm scheduler.
class ActorStorage final {
 public:
  class Impl;
  explicit ActorStorage(kj::Own<Impl> impl);
  ~ActorStorage() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ActorStorage);

  Impl& getImpl() const;

 private:
  mutable kj::Own<Impl> impl;
};

kj::Own<ActorStorage> factory_new_actor_storage(const WorkerFactory& factory,
    ::rust::Str path,
    ::rust::Str uniqueKey,
    ::rust::Box<ActorNamespaceHandle> actors);
void actor_storage_delete_all(const ActorStorage& storage);
// Facets have numbers only in a directory (SQLite storage). In-memory storage is an ActorCache per
// actor, in which they have none, and nothing to delete or clone.
void actor_storage_delete_facet(const ActorStorage& storage,
    ::rust::Str rootKey,
    kj::Maybe<uint32_t> parentFacetId,
    ::rust::Str name);
void actor_storage_clone_facet(const ActorStorage& storage,
    ::rust::Str rootKey,
    kj::Maybe<uint32_t> parentFacetId,
    ::rust::Str src,
    ::rust::Str dst);
kj::Maybe<uint32_t> actor_storage_facet_id(const ActorStorage& storage,
    ::rust::Str rootKey,
    kj::Maybe<uint32_t> parentFacetId,
    ::rust::Str name);

kj::Promise<void> factory_shutdown_containers(const WorkerFactory& factory);

// =======================================================================================

kj::Own<RequestMetadata> new_request_metadata(
    kj::Maybe<::rust::Str> cfBlobJson, kj::Maybe<::rust::Str> clientAddress);
kj::Maybe<::rust::String> request_metadata_cf_blob_json(const RequestMetadata& metadata);
void request_metadata_set_from_persistent_stub(RequestMetadata& metadata, bool persistent);
void request_metadata_set_actor_self_token(const WorkerFactory& factory,
    RequestMetadata& metadata,
    ::rust::Str uniqueKey,
    const ActorIdHandle& id,
    bool persistent);

void exception_throw(const kj::Exception& exception);
::rust::String exception_text(const AbortReason& error);

kj::Own<Frankenvalue> frankenvalue_from_json(::rust::Str json);
kj::Own<Frankenvalue> frankenvalue_clone(const Frankenvalue& value);
bool frankenvalue_is_empty(const Frankenvalue& value);
kj::Own<Frankenvalue> frankenvalue_new();
void frankenvalue_set_service_stub(
    Frankenvalue& value, ::rust::Str name, ::rust::Box<SubrequestChannel> channel);

// =======================================================================================

// The server's channel objects as KJ channels, which the runtime holds as `kj::Rc` of the KJ
// interface.
class SubrequestChannelHandle final: public IoChannelFactory::SubrequestChannel {
 public:
  explicit SubrequestChannelHandle(::rust::Box<server::SubrequestChannel> channel);

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override;
  kj::Promise<void> evictForTest(IoChannelFactory::EvictWebSocketMode mode) override;
  void requireAllowsTransfer() override;
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override;

 private:
  ::rust::Box<server::SubrequestChannel> channel;
};

class ActorClassChannelHandle final: public IoChannelFactory::ActorClassChannel {
 public:
  explicit ActorClassChannelHandle(::rust::Box<server::ActorClassChannel> channel);

  // The Rust handle of a class the server made. A facet's class comes back from the runtime in
  // the facet's start info; unwrapping it yields the same Rust object, not a wrapper of a wrapper.
  static kj::Maybe<::rust::Box<server::ActorClassChannel>> tryUnwrap(
      IoChannelFactory::ActorClassChannel& channel);

  void requireAllowsTransfer() override;
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override;

 private:
  ::rust::Box<server::ActorClassChannel> channel;
};

kj::Rc<SubrequestChannelHandle> subrequest_channel_into_kj(::rust::Box<SubrequestChannel> channel);
kj::Rc<ActorClassChannelHandle> actor_class_channel_into_kj(::rust::Box<ActorClassChannel> channel);

// =======================================================================================

kj::Promise<void> factory_accept_bootstrap(const WorkerFactory& factory,
    kj::Own<kj::AsyncIoStream> stream,
    ::rust::Box<SubrequestChannel> target);
kj::Promise<void> factory_accept_debug_port(
    const WorkerFactory& factory, kj::Own<kj::AsyncIoStream> stream);

// The client side of a capnp-over-HTTP-CONNECT connection.
class RpcClient final {
 public:
  class Impl;
  explicit RpcClient(kj::Own<Impl> impl);
  ~RpcClient() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(RpcClient);

  Impl& getImpl() const;

 private:
  mutable kj::Own<Impl> impl;
};

kj::Own<RpcClient> new_rpc_client(const WorkerFactory& factory, kj::Own<kj::AsyncIoStream> stream);
kj::Promise<rust::worker::CustomEventResult> rpc_client_custom_event(const RpcClient& client,
    kj::Own<WorkerInterface::CustomEvent> event,
    kj::Maybe<::rust::Str> cfBlobJson);
kj::Promise<void> rpc_client_on_disconnect(const RpcClient& client);

uint16_t factory_start_inspector(const WorkerFactory& factory, ::rust::Str address);

// The listeners' shims (worker-factory-listen.c++).
kj::Own<kj::HttpHeaders> edit_headers(const kj::HttpHeaderTable& table,
    const kj::HttpHeaders& headers,
    ::rust::Slice<const HeaderEdit> edits,
    ::rust::Slice<const HeaderEdit> injected);
kj::Own<kj::HttpService::Response> new_rewriting_response(kj::HttpService::Response& inner,
    const kj::HttpHeaderTable& table,
    ::rust::Slice<const HeaderEdit> edits);
kj::Own<kj::HttpService::ConnectResponse> new_null_connect_response();
kj::Own<kj::WebSocketErrorHandler> new_jsgify_websocket_errors();
kj::Own<WorkerInterface::CustomEvent> new_udp_connect_event(
    ::rust::Str address, ::rust::Box<UdpFlow> flow);

// Channel tokens, through the factory's ChannelTokenHandler.
kj::Own<PendingToken> factory_encode_subrequest_token(const WorkerFactory& factory,
    ::rust::Str serviceName,
    kj::Maybe<::rust::Str> entrypoint,
    kj::Maybe<const Frankenvalue&> props,
    bool persistent,
    TokenUsage usage);
kj::Own<PendingToken> factory_encode_actor_class_token(const WorkerFactory& factory,
    ::rust::Str serviceName,
    ::rust::Str className,
    kj::Maybe<const Frankenvalue&> props,
    bool persistent,
    TokenUsage usage);
kj::Own<PendingToken> factory_encode_actor_token(const WorkerFactory& factory,
    ::rust::Str uniqueKey,
    const ActorIdHandle& id,
    bool persistent,
    TokenUsage usage);

}  // namespace workerd::server
