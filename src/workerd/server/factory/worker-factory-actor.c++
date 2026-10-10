// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-factory-impl.h"

#include <workerd/api/actor-state.h>
#include <workerd/io/actor-cache.h>
#include <workerd/io/io-context.h>
#include <workerd/server/actor-id-impl.h>
#include <workerd/server/workerd-api.h>

namespace workerd::server {

// =======================================================================================
// ActorStorage

ActorStorage::Impl::Impl(const WorkerFactory& factory,
    kj::Maybe<kj::Own<const kj::Directory>> directoryParam,
    kj::String uniqueKey,
    ::rust::Box<ActorNamespaceHandle> actors)
    : directory(kj::mv(directoryParam)),
      vfsDirectory(directory.map([](kj::Own<const kj::Directory>& d) { return d->clone(); })
                       .orDefault([&]() {
                         return kj::newInMemoryDirectory(kj::systemPreciseCalendarClock());
                       })),
      vfs(*vfsDirectory),
      uniqueKey(kj::mv(uniqueKey)),
      alarmScheduler(kj::heap<AlarmScheduler>(kj::systemPreciseCalendarClock(),
          factory.getImpl().timer,
          vfs,
          kj::Path({"metadata.sqlite"}),
          [uniqueKey = this->uniqueKey.asPtr(), actors = kj::mv(actors)](
              const ActorKey& actor) mutable -> kj::Own<WorkerInterface> {
            // The id carries the persisted name so that `ctx.id.name` is set when the alarm runs.
            auto id = ActorIdFactoryImpl(uniqueKey).idFromStringNamed(
                kj::str(actor.actorId), actor.name.map([](kj::StringPtr n) { return kj::str(n); }));
            return actors->actor_for_alarm(kj::heap<ActorIdHandle>(kj::mv(id)));
          })) {}

kj::Maybe<kj::Own<FacetTreeIndex>> ActorStorage::Impl::getFacetTreeIndexIfNotEmpty(
    const kj::Directory& dir, kj::StringPtr rootKey) {
  return dir.tryOpenFile(kj::Path({kj::str(rootKey, ".facets")}), kj::WriteMode::MODIFY)
      .map([](kj::Own<const kj::File>&& file) { return kj::heap<FacetTreeIndex>(kj::mv(file)); });
}

kj::Path ActorStorage::Impl::getSqlitePath(
    kj::StringPtr rootKey, uint facetId, kj::StringPtr suffix) {
  if (facetId == 0) {
    return kj::Path({kj::str(rootKey, ".sqlite", suffix)});
  }
  return kj::Path({kj::str(rootKey, '.', facetId, ".sqlite", suffix)});
}

void ActorStorage::Impl::deleteFacet(
    const kj::Directory& dir, kj::StringPtr rootKey, FacetTreeIndex& index, uint facetId) {
  index.forEachChild(
      facetId, [&](uint childId, kj::StringPtr) { deleteFacet(dir, rootKey, index, childId); });
  // The database may not exist at all if the facet never ran.
  dir.tryRemove(getSqlitePath(rootKey, facetId));
  dir.tryRemove(getSqlitePath(rootKey, facetId, "-wal"));
  dir.tryRemove(getSqlitePath(rootKey, facetId, "-shm"));
}

void ActorStorage::Impl::deleteDescendantStorage(
    const kj::Directory& dir, kj::StringPtr rootKey, uint parentId) {
  KJ_IF_SOME(index, getFacetTreeIndexIfNotEmpty(dir, rootKey)) {
    index->forEachChild(
        parentId, [&](uint childId, kj::StringPtr) { deleteFacet(dir, rootKey, *index, childId); });
  } else {
    // No index, so no facets other than the root.
    KJ_ASSERT(parentId == 0);
  }
}

void ActorStorage::Impl::cloneFacet(const kj::Directory& dir,
    kj::StringPtr rootKey,
    FacetTreeIndex& index,
    uint srcId,
    uint dstId) {
  // Snapshot the children first: allocating the destination's ids mutates the index.
  struct Child {
    uint id;
    kj::String name;
  };
  kj::Vector<Child> children;
  index.forEachChild(srcId, [&](uint childId, kj::StringPtr childName) {
    children.add(Child{childId, kj::str(childName)});
  });
  for (auto& child: children) {
    cloneFacet(dir, rootKey, index, child.id, index.getId(dstId, child.name));
  }

  // A source without a database has no data, which is what the destination already has. The WAL
  // is copied with the database since a crashed process may have left one uncheckpointed; the
  // SHM only speeds up the first open.
  for (const auto& suffix: {""_kj, "-wal"_kj, "-shm"_kj}) {
    auto src = getSqlitePath(rootKey, srcId, suffix);
    if (!dir.exists(src)) return;
    dir.transfer(
        getSqlitePath(rootKey, dstId, suffix), kj::WriteMode::CREATE, src, kj::TransferMode::COPY);
  }
}

ActorStorage::ActorStorage(kj::Own<Impl> impl): impl(kj::mv(impl)) {}
ActorStorage::~ActorStorage() noexcept(false) = default;
ActorStorage::Impl& ActorStorage::getImpl() const {
  return *impl;
}

kj::Own<ActorStorage> factory_new_actor_storage(const WorkerFactory& factory,
    ::rust::Str path,
    ::rust::Str uniqueKey,
    ::rust::Box<ActorNamespaceHandle> actors) {
  kj::Maybe<kj::Own<const kj::Directory>> directory;
  if (!path.empty()) {
    auto& fs = factory.getImpl().fs;
    auto base = fs.getRoot().openSubdir(fs.getCurrentPath().evalNative(kj::str(path)),
        kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
    directory = base->openSubdir(
        kj::Path({kj::str(uniqueKey)}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
  }
  return kj::heap<ActorStorage>(kj::refcounted<ActorStorage::Impl>(
      factory, kj::mv(directory), kj::str(uniqueKey), kj::mv(actors)));
}

void actor_storage_delete_all(const ActorStorage& storage) {
  storage.getImpl().alarmScheduler->deleteAll();
}

void actor_storage_delete_facet(const ActorStorage& storage,
    ::rust::Str rootKey,
    kj::Maybe<uint32_t> parentFacetId,
    ::rust::Str name) {
  auto& impl = storage.getImpl();
  auto& dir = *KJ_UNWRAP_OR(impl.directory, return);
  auto key = kj::str(rootKey);
  // Without an index there can be no facet storage.
  KJ_IF_SOME(index, impl.getFacetTreeIndexIfNotEmpty(dir, key)) {
    impl.deleteFacet(dir, key, *index, index->getId(parentFacetId.orDefault(0), kj::str(name)));
  }
}

void actor_storage_clone_facet(const ActorStorage& storage,
    ::rust::Str rootKey,
    kj::Maybe<uint32_t> parentFacetId,
    ::rust::Str src,
    ::rust::Str dst) {
  auto& impl = storage.getImpl();
  auto& dir = *KJ_UNWRAP_OR(impl.directory, return);
  auto key = kj::str(rootKey);
  // Without an index there is no storage to delete or copy.
  KJ_IF_SOME(index, impl.getFacetTreeIndexIfNotEmpty(dir, key)) {
    uint parentId = parentFacetId.orDefault(0);
    // The destination's storage goes first, as deleting the facet would; then the copy. A source
    // without a database has no data, which the emptied destination matches.
    uint dstId = index->getId(parentId, kj::str(dst));
    impl.deleteFacet(dir, key, *index, dstId);
    impl.cloneFacet(dir, key, *index, index->getId(parentId, kj::str(src)), dstId);
  }
}

kj::Maybe<uint32_t> actor_storage_facet_id(const ActorStorage& storage,
    ::rust::Str rootKey,
    kj::Maybe<uint32_t> parentFacetId,
    ::rust::Str name) {
  auto& dir = *KJ_UNWRAP_OR(storage.getImpl().directory, return kj::none);
  // The index is read from disk for this one lookup; the server keeps the number it yields.
  auto file = dir.openFile(
      kj::Path({kj::str(rootKey, ".facets")}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
  return FacetTreeIndex(kj::mv(file)).getId(parentFacetId.orDefault(0), kj::str(name));
}

// =======================================================================================
// Actors

namespace {

// The actor's hooks into the server: the requests it raises for itself (alarms, hibernated
// WebSocket events, which restart the actor if needed), its facets, and its transitions between
// idle and active. The actor owns a reference as its loopback, so the hooks it borrows as its
// facet manager live as long as it does.
class ActorHooksImpl final: public Worker::Actor::Loopback,
                            public Worker::Actor::FacetManager,
                            public RequestTracker::Hooks,
                            public kj::Refcounted {
 public:
  explicit ActorHooksImpl(::rust::Box<ActorHooks> hooks): hooks(kj::mv(hooks)) {}

  kj::Own<WorkerInterface> getWorker(IoChannelFactory::SubrequestMetadata metadata) override {
    return hooks->start_request(kj::heap<RequestMetadata>(kj::mv(metadata)));
  }
  kj::Own<Worker::Actor::Loopback> addRef() override {
    return kj::addRef(*this);
  }

  uint getDepth() const override {
    return hooks->depth();
  }
  kj::Rc<IoChannelFactory::ActorChannel> getFacet(
      kj::StringPtr name, kj::Function<kj::Promise<StartInfo>()> getStartInfo) override {
    return subrequest_channel_into_kj(
        hooks->facet(toRust(name), kj::heap<FacetStart>(kj::mv(getStartInfo))));
  }
  void abortFacet(kj::StringPtr name, kj::Exception reason) override {
    hooks->abort_facet(toRust(name), reason);
  }
  void deleteFacet(kj::StringPtr name) override {
    hooks->delete_facet(toRust(name));
  }
  void cloneFacet(kj::StringPtr src, kj::StringPtr dst) override {
    hooks->clone_facet(toRust(src), toRust(dst));
  }

  void active() override {
    hooks->active();
  }
  void inactive() override {
    hooks->inactive();
  }

 private:
  ::rust::Box<ActorHooks> hooks;
};

// Schedules a root actor's alarms with the namespace's scheduler; the prior task is ignored since
// everything runs synchronously here.
class ActorSqliteHooks final: public ActorSqlite::Hooks {
 public:
  ActorSqliteHooks(kj::Own<ActorStorage::Impl> storage, ActorKey actor)
      : storage(kj::mv(storage)),
        actor(kj::mv(actor)) {}

  kj::Promise<void> scheduleRun(
      kj::Maybe<kj::Date> newAlarmTime, kj::Promise<void> priorTask) override {
    KJ_IF_SOME(scheduledTime, newAlarmTime) {
      storage->alarmScheduler->setAlarm(actor, scheduledTime);
    } else {
      storage->alarmScheduler->deleteAlarm(actor);
    }
    return kj::READY_NOW;
  }

 private:
  kj::Own<ActorStorage::Impl> storage;
  ActorKey actor;
};

// Facets have their own storage but no alarms: the scheduler delivers only to root actors.
class FacetAlarmHooks final: public ActorSqlite::Hooks {
 public:
  kj::Promise<void> scheduleRun(
      kj::Maybe<kj::Date> newAlarmTime, kj::Promise<void> priorTask) override {
    // The same message as the production storage factory's.
    JSG_FAIL_REQUIRE(Error, "Facets currently cannot set alarms.");
  }
};

// The actor's storage: SQLite in the namespace's directory, or an in-memory cache over empty
// storage when the namespace has no directory. None for an ephemeral actor.
Worker::Actor::MakeActorCacheFunc makeActorCacheFunc(kj::Own<ActorStorage::Impl> storage,
    const ActorStorageSpec& spec,
    kj::String rootKey,
    kj::Maybe<kj::String> actorName) {
  return [storage = kj::mv(storage), durable = spec.durable, facetId = spec.facet_id,
             rootKey = kj::mv(rootKey),
             actorName = kj::mv(actorName)](const ActorCache::SharedLru& sharedLru,
             OutputGate& outputGate, ActorCache::Hooks& hooks,
             SqliteObserver& sqliteObserver) mutable -> kj::Maybe<kj::Own<ActorCacheInterface>> {
    if (!durable) return kj::none;
    if (storage->directory == kj::none) {
      // The cache never flushes (see NullIsolateLimitEnforcer), so this is in-memory storage.
      return kj::heap<ActorCache>(newEmptyReadOnlyActorStorage(), sharedLru, outputGate, hooks);
    }

    kj::Own<ActorSqlite::Hooks> sqliteHooks;
    uint selfId = facetId.orDefault(0);
    if (facetId == kj::none) {
      sqliteHooks =
          kj::heap<ActorSqliteHooks>(kj::addRef(*storage), ActorKey(rootKey.asPtr(), actorName));
    } else {
      sqliteHooks = kj::heap<FacetAlarmHooks>();
    }

    auto db = kj::heap<SqliteDatabase>(storage->vfs, storage->getSqlitePath(rootKey, selfId),
        kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
    // The database runs in WAL mode, also after `reset()` (which `deleteAll()` uses, and which
    // also deletes the child facets' storage; that is not transactional with the reset, as it is
    // in production).
    db->run("PRAGMA journal_mode=WAL;");
    db->afterReset([storage = kj::addRef(*storage), rootKey = kj::str(rootKey), selfId](
                       SqliteDatabase& db) mutable {
      db.run("PRAGMA journal_mode=WAL;");
      storage->deleteDescendantStorage(*KJ_ASSERT_NONNULL(storage->directory), rootKey, selfId);
    });
    return kj::heap<ActorSqlite>(kj::mv(db), outputGate,
        [](SpanParent) -> kj::Promise<void> { return kj::READY_NOW; }, *sqliteHooks)
        .attach(kj::mv(sqliteHooks));
  };
}

// The Docker client of the container `id`: the live one, if an earlier incarnation of the actor
// left it running (a `setInactivityTimeout()` timer holds it), else a new one for the class's
// `container` options on its worker's `containerEngine`.
kj::Own<ContainerClient> getContainerClient(WorkerFactory::Impl& factory,
    kj::String id,
    config::Worker::ContainerEngine::Reader engine,
    config::Worker::DurableObjectNamespace::ContainerOptions::Reader options) {
  KJ_REQUIRE(!factory.containerShutdownStarted,
      "cannot acquire a container client after graceful shutdown has begun");
  auto& state = *factory.containers.findOrCreate(id, [&]() {
    return decltype(factory.containers)::Entry{
      kj::str(id), kj::heap<WorkerFactory::Impl::Container>()};
  });
  KJ_IF_SOME(client, state.client) {
    return client.addRef();
  }

  KJ_REQUIRE(engine.isLocalDocker(),
      "dockerPath must be defined to enable containers on this Durable Object.");
  auto docker = engine.getLocalDocker();
  KJ_REQUIRE(docker.hasContainerEgressInterceptorImage(),
      "containerEgressInterceptorImage must be configured for containers.");
  kj::Maybe<kj::String> imageName;
  if (options.getImageName().size() > 0) imageName = kj::str(options.getImageName());
  auto privilegeConf = options.getPrivileges();
  ContainerPrivileges privileges{
    .capabilities = KJ_MAP(c, privilegeConf.getCapabilities()) { return kj::str(c); },
    .devices =
        KJ_MAP(device, privilegeConf.getDevices()) {
    return ContainerPrivileges::Device{
      .pathOnHost = kj::str(device.getPathOnHost()),
      .pathInContainer = kj::str(device.getPathInContainer()),
      .cgroupPermissions = kj::str(device.getCgroupPermissions()),
    };
  },
    .securityOpt = KJ_MAP(o, privilegeConf.getSecurityOpt()) { return kj::str(o); },
  };

  // Docker may still be removing the container for the client before this one, which would race
  // this client: that removal is cancelled, and this client waits for it to have ended.
  auto previousCleanup = state.cleanup.addBranch();
  state.canceler.cancel("a new container client took the container over"_kj);
  auto client = kj::refcounted<ContainerClient>(factory.byteStreamFactory, factory.timer,
      factory.network, kj::str(docker.getSocketPath()), kj::mv(id), kj::mv(imageName),
      kj::str(docker.getContainerEgressInterceptorImage()), factory.tasks, kj::mv(previousCleanup),
      [&state, &tasks = factory.tasks](kj::Promise<void> cleanup) {
    // The client's shutdown began: the next incarnation of the actor gets a new client.
    state.client = kj::none;
    state.cleanup = state.canceler.wrap(kj::mv(cleanup)).catch_([](kj::Exception&&) {}).fork();
    tasks.add(state.cleanup.addBranch());
  }, factory.channelTokenHandler, kj::mv(privileges));
  state.client = *client;
  return client;
}

}  // namespace

ActorHandle::ActorHandle(kj::Own<Worker::Actor::FacetManager> hooks,
    kj::Own<RequestTracker> tracker,
    kj::Own<Worker::Actor> actor)
    : hooks(kj::mv(hooks)),
      tracker(kj::mv(tracker)),
      actor(kj::mv(actor)) {}
ActorHandle::~ActorHandle() noexcept(false) {
  tracker->shutdown();
}
Worker::Actor& ActorHandle::getActor() const {
  return *actor;
}
kj::Own<Worker::Actor> ActorHandle::addRef() const {
  return actor->addRef();
}

kj::Own<ActorHandle> worker_new_actor(const CompiledWorker& worker,
    ::rust::Str className,
    kj::Maybe<kj::Own<Frankenvalue>> props,
    kj::Own<ActorIdHandle> id,
    const ActorStorage& storage,
    const ActorStorageSpec& spec,
    ::rust::Box<ActorHooks> hooks,
    kj::Maybe<kj::Rc<HibernationManager>> hibernationManager,
    kj::Maybe<const ContainerRef&> container) {
  auto& impl = worker.getImpl();
  auto actorId = Worker::Actor::cloneId(*id);

  // The name the actor was created with (`idFromName()`), for the alarm scheduler to persist so
  // that `ctx.id.name` is restored when the alarm fires after an eviction.
  kj::Maybe<kj::String> actorName;
  KJ_IF_SOME(doId, actorId.tryGet<kj::Own<ActorIdFactory::ActorId>>()) {
    actorName = doId->getName().map([](kj::StringPtr n) { return kj::str(n); });
  }
  auto makeActorCache = makeActorCacheFunc(
      kj::addRef(storage.getImpl()), spec, kj::str(spec.root_key), kj::mv(actorName));

  auto makeStorage = [enableSql = spec.enable_sql](jsg::Lock& js, const Worker::Api& api,
                         ActorCacheInterface& actorCache) -> jsg::Ref<api::DurableObjectStorage> {
    return js.alloc<api::DurableObjectStorage>(
        js, IoContext::current().addObject(actorCache), enableSql);
  };

  Frankenvalue propsValue;
  KJ_IF_SOME(p, props) propsValue = kj::mv(*p);

  jsg::Dict<kj::String> images;
  kj::Maybe<rpc::Container::Client> containerClient;
  KJ_IF_SOME(ref, container) {
    auto& factory = impl.factory.getImpl();
    auto workerConf = factory.config.getServices()[ref.service_index].getWorker();
    auto options = workerConf.getDurableObjectNamespaces()[ref.namespace_index].getContainer();
    images.fields = KJ_MAP(image, options.getImages()) {
      return jsg::Dict<kj::String>::Field{
        .name = kj::str(image.getName()), .value = kj::str(image.getImage())};
    };
    // Unique per namespace and actor across the machine.
    containerClient = rpc::Container::Client(getContainerClient(factory,
        kj::str("workerd-", storage.getImpl().uniqueKey, "-", actor_id_key(*id)),
        workerConf.getContainerEngine(), options));
  }

  auto actorHooks = kj::refcounted<ActorHooksImpl>(kj::mv(hooks));
  auto requestTracker = kj::refcounted<RequestTracker>(*actorHooks);
  auto classNameStr = kj::str(className);
  // The hibernation event type id is defined outside workerd; WebSocket hibernation needs one.
  static constexpr uint16_t hibernationEventTypeId = 8;
  auto actor = kj::refcounted<Worker::Actor>(impl.getWorker(), *requestTracker, kj::mv(actorId),
      true, kj::mv(makeActorCache), classNameStr.asPtr(), kj::mv(propsValue), kj::mv(makeStorage),
      kj::addRef(*actorHooks), impl.timerChannel, kj::refcounted<ActorObserver>(),
      hibernationManager.map([](kj::Rc<HibernationManager>& m) { return m.toOwn(); }),
      hibernationEventTypeId, kj::mv(containerClient), kj::mv(images), *actorHooks);
  return kj::heap<ActorHandle>(kj::mv(actorHooks), kj::mv(requestTracker), kj::mv(actor));
}

kj::Own<ActorBroken> actor_on_broken(const ActorHandle& actor) {
  return kj::heap(actor.getActor().onBroken());
}

kj::Promise<void> actor_broken(kj::Own<ActorBroken> broken) {
  return kj::mv(*broken);
}

void actor_abort(const ActorHandle& actor, const AbortReason& reason) {
  // Raising the reason across the bridge is the bridge's own KjError -> kj::Exception conversion.
  KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() { reason.raise(); })) {
    actor.getActor().abort(exception);
  } else {
    actor.getActor().shutdown(0, kj::none);
  }
}

kj::Promise<kj::Own<ActorLock>> actor_lock(const ActorHandle& actor) {
  co_return kj::heap<ActorLock>(
      co_await actor.getActor().getWorker().takeAsyncLockWithoutRequest(nullptr));
}

bool actor_shutdown(
    const ActorHandle& handle, ::rust::Str reason, kj::Maybe<kj::Own<ActorLock>> lock) {
  auto& actor = handle.getActor();
  if (actor.isShared()) return false;
  KJ_IF_SOME(asyncLock, lock) {
    KJ_IF_SOME(manager, actor.getHibernationManager()) {
      actor.getWorker().runInLockScope(
          *asyncLock, [&](Worker::Lock& lock) { manager.hibernateWebSockets(lock); });
    }
  }
  actor.shutdown(0, KJ_EXCEPTION(DISCONNECTED, kj::str(reason)));
  return true;
}

void actor_reset_storage(const ActorHandle& actor) {
  KJ_IF_SOME(cache, actor.getActor().getPersistent()) {
    KJ_IF_SOME(db, cache.getSqliteDatabase()) {
      kj::runCatchingExceptions([&]() { db.reset(); });
    }
  }
}

kj::Maybe<kj::Rc<HibernationManager>> actor_hibernation_manager(const ActorHandle& actor) {
  return actor.getActor().getHibernationManager().map(
      [](HibernationManager& m) { return kj::Rc<HibernationManager>(m.addRef()); });
}

kj::Promise<FacetStartInfo> facet_start_resolve(kj::Own<FacetStart> start) {
  auto info = co_await (*start)();
  co_await info.ensureAllResolved();
  co_return FacetStartInfo{
    .id = kj::heap<ActorIdHandle>(kj::mv(info.id)),
    .actor_class = KJ_REQUIRE_NONNULL(ActorClassChannelHandle::tryUnwrap(*info.actorClass),
        "a facet's class must be one the server made"),
  };
}

// =======================================================================================
// Containers

kj::Promise<void> factory_shutdown_containers(const WorkerFactory& factory) {
  auto& impl = factory.getImpl();
  impl.containerShutdownStarted = true;
  kj::Vector<kj::Promise<void>> cleanups(impl.containers.size());
  for (auto& entry: impl.containers) {
    // A client's shutdown replaces its entry's `cleanup`.
    KJ_IF_SOME(client, entry.value->client) client.shutdown();
    cleanups.add(entry.value->cleanup.addBranch());
  }
  return kj::joinPromises(cleanups.releaseAsArray());
}

}  // namespace workerd::server
