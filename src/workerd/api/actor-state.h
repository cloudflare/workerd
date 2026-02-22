// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// APIs that an Actor (Durable Object) uses to access its own state.
//
// See actor.h for APIs used by other Workers to talk to Actors.

#include <workerd/api/actor.h>
#include <workerd/api/container.h>
#include <workerd/io/actor-cache.h>
#include <workerd/io/actor-id.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/io-own.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/jsg.h>

#include <kj/async.h>

namespace workerd::api {
class SqlStorage;
class SyncKvStorage;

// Forward-declared to avoid dependency cycle (actor.h -> http.h -> basics.h -> actor-state.h)
class DurableObject;
class DurableObjectId;
class WebSocket;
class DurableObjectClass;
class LoopbackDurableObjectNamespace;
class LoopbackColoLocalActorNamespace;

kj::Array<kj::byte> serializeV8Value(jsg::Lock& js, const jsg::JsValue& value);

jsg::JsValue deserializeV8Value(
    jsg::Lock& js, kj::ArrayPtr<const char> key, kj::ArrayPtr<const kj::byte> buf);

// Common implementation of DurableObjectStorage and DurableObjectTransaction. This class is
// designed to be used as a mixin.
class DurableObjectStorageOperations {
 public:
  struct GetOptions {
    jsg::Optional<bool> allowConcurrency;
    jsg::Optional<bool> noCache;

    inline operator ActorCacheOps::ReadOptions() const {
      return {.noCache = noCache.orDefault(false)};
    }

    JSG_STRUCT(allowConcurrency, noCache);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectGetOptions);  // Rename from DurableObjectStorageOperationsGetOptions
  };

  jsg::Promise<jsg::JsRef<jsg::JsValue>> get(jsg::Lock& js,
      kj::OneOf<kj::String, kj::Array<kj::String>> keys,
      jsg::Optional<GetOptions> options);

  struct GetAlarmOptions {
    jsg::Optional<bool> allowConcurrency;

    JSG_STRUCT(allowConcurrency);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectGetAlarmOptions);  // Rename from DurableObjectStorageOperationsGetAlarmOptions
  };

  jsg::Promise<kj::Maybe<double>> getAlarm(jsg::Lock& js, jsg::Optional<GetAlarmOptions> options);

  struct ListOptions {
    jsg::Optional<kj::String> start;
    jsg::Optional<kj::String> startAfter;
    jsg::Optional<kj::String> end;
    jsg::Optional<kj::String> prefix;
    jsg::Optional<bool> reverse;
    jsg::Optional<int> limit;

    jsg::Optional<bool> allowConcurrency;
    jsg::Optional<bool> noCache;

    inline operator ActorCacheOps::ReadOptions() const {
      return {.noCache = noCache.orDefault(false)};
    }

    JSG_STRUCT(start, startAfter, end, prefix, reverse, limit, allowConcurrency, noCache);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectListOptions);  // Rename from DurableObjectStorageOperationsListOptions
  };

  // A more convenient form of `ListOptions` for actually implementing the operation -- but less
  // convenient for specifying it.
  struct CompiledListOptions {
    kj::String start;
    kj::Maybe<kj::String> end;
    bool reverse;
    kj::Maybe<uint> limit;
  };

  // Compile `ListOptions` into `CompiledListOptions`. Returns null if the list operation would
  // provably return no results (e.g. the end key is before the start key). This may (or may not)
  // move some of the strings from the input to the output.
  //
  // This is public so that SyncKvStorage can reuse it.
  static kj::Maybe<CompiledListOptions> compileListOptions(kj::Maybe<ListOptions>& maybeOptions);

  jsg::Promise<jsg::JsRef<jsg::JsValue>> list(jsg::Lock& js, jsg::Optional<ListOptions> options);

  struct PutOptions {
    jsg::Optional<bool> allowConcurrency;
    jsg::Optional<bool> allowUnconfirmed;
    jsg::Optional<bool> noCache;

    inline operator ActorCacheOps::WriteOptions() const {
      return {
        .allowUnconfirmed = allowUnconfirmed.orDefault(false), .noCache = noCache.orDefault(false)};
    }

    JSG_STRUCT(allowConcurrency, allowUnconfirmed, noCache);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectPutOptions);  // Rename from DurableObjectStorageOperationsPutOptions
  };

  jsg::Promise<void> put(jsg::Lock& js,
      kj::OneOf<kj::String, jsg::Dict<jsg::JsValue>> keyOrEntries,
      jsg::Optional<jsg::JsValue> value,
      jsg::Optional<PutOptions> options,
      const jsg::TypeHandler<PutOptions>& optionsTypeHandler);

  kj::OneOf<jsg::Promise<bool>, jsg::Promise<int>> delete_(jsg::Lock& js,
      kj::OneOf<kj::String, kj::Array<kj::String>> keys,
      jsg::Optional<PutOptions> options);

  struct SetAlarmOptions {
    jsg::Optional<bool> allowConcurrency;
    jsg::Optional<bool> allowUnconfirmed;
    // We don't allow noCache for alarm puts.

    inline operator ActorCacheOps::WriteOptions() const {
      return {
        .allowUnconfirmed = allowUnconfirmed.orDefault(false),
      };
    }

    JSG_STRUCT(allowConcurrency, allowUnconfirmed);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectSetAlarmOptions);  // Rename from DurableObjectStorageOperationsSetAlarmOptions
  };

  jsg::Promise<void> setAlarm(
      jsg::Lock& js, kj::Date scheduledTime, jsg::Optional<SetAlarmOptions> options);
  jsg::Promise<void> deleteAlarm(jsg::Lock& js, jsg::Optional<SetAlarmOptions> options);

 protected:
  using OpName = kj::StringPtr;
  static constexpr OpName OP_GET = "get()"_kj;
  static constexpr OpName OP_GET_ALARM = "getAlarm()"_kj;
  static constexpr OpName OP_LIST = "list()"_kj;
  static constexpr OpName OP_PUT = "put()"_kj;
  static constexpr OpName OP_PUT_ALARM = "setAlarm()"_kj;
  static constexpr OpName OP_DELETE = "delete()"_kj;
  static constexpr OpName OP_DELETE_ALARM = "deleteAlarm()"_kj;
  static constexpr OpName OP_RENAME = "rename()"_kj;
  static constexpr OpName OP_ROLLBACK = "rollback()"_kj;

  static bool readOnlyOp(OpName op) {
    return op == OP_GET || op == OP_LIST || op == OP_ROLLBACK;
  }

  virtual ActorCacheOps& getCache(OpName op) = 0;

  // Whether to skip caching and allow concurrency on all operations.
  virtual bool useDirectIo() = 0;

  // Method that should be called at the start of each storage operation to override any of the
  // options as appropriate.
  template <typename T>
  T configureOptions(T&& options) {
    if (useDirectIo()) {
      options.allowConcurrency = true;
      options.noCache = true;
    }
    return kj::mv(options);
  }

 private:
  jsg::Promise<jsg::JsRef<jsg::JsValue>> getOne(
      jsg::Lock& js, kj::String key, const GetOptions& options);
  jsg::Promise<jsg::JsRef<jsg::JsValue>> getMultiple(
      jsg::Lock& js, kj::Array<kj::String> keys, const GetOptions& options);

  jsg::Promise<void> putOne(
      jsg::Lock& js, kj::String key, jsg::JsValue value, const PutOptions& options);
  jsg::Promise<void> putMultiple(
      jsg::Lock& js, jsg::Dict<jsg::JsValue> entries, const PutOptions& options);

  jsg::Promise<bool> deleteOne(jsg::Lock& js, kj::String key, const PutOptions& options);
  jsg::Promise<int> deleteMultiple(
      jsg::Lock& js, kj::Array<kj::String> keys, const PutOptions& options);
};

class DurableObjectTransaction;

class DurableObjectStorage: public jsg::Object, public DurableObjectStorageOperations {
 public:
  DurableObjectStorage(jsg::Lock&, IoPtr<ActorCacheInterface> cache, bool enableSql)
      : cache(kj::mv(cache)),
        enableSql(enableSql) {}

  // This constructor is only used when we're setting up the `DurableObjectStorage` for a replica
  // Durable Object instance. Replicas need to retain a reference to their primary so they can
  // forward write requests, and since we already have a reference to the primary prior to
  // constructing the `DurableObjectStorage`, we can just pass in the information we need to build
  // a stub. The stub is then stored in `maybePrimary`.
  DurableObjectStorage(jsg::Lock& js,
      IoPtr<ActorCacheInterface> cache,
      bool enableSql,
      kj::Own<IoChannelFactory::ActorChannel> primaryActorChannel,
      kj::Own<ActorIdFactory::ActorId> primaryActorId);

  ActorCacheInterface& getActorCacheInterface() {
    return *cache;
  }

  // Throws if not SQLite-backed.
  SqliteDatabase& getSqliteDb(jsg::Lock& js);
  SqliteKv& getSqliteKv(jsg::Lock& js);

  struct TransactionOptions {
    jsg::Optional<kj::Date> asOfTime;
    jsg::Optional<bool> lowPriority;

    JSG_STRUCT(asOfTime, lowPriority);
    JSG_STRUCT_TS_OVERRIDE(type TransactionOptions = never);
    // Omit from definitions
  };

  jsg::Promise<jsg::JsRef<jsg::JsValue>> transaction(jsg::Lock& js,
      jsg::Function<jsg::Promise<jsg::JsRef<jsg::JsValue>>(jsg::Ref<DurableObjectTransaction>)>
          closure,
      jsg::Optional<TransactionOptions> options);

  jsg::JsRef<jsg::JsValue> transactionSync(
      jsg::Lock& js, jsg::Function<jsg::JsRef<jsg::JsValue>()> callback);

  jsg::Promise<void> deleteAll(jsg::Lock& js, jsg::Optional<PutOptions> options);

  jsg::Promise<void> sync(jsg::Lock& js);

  jsg::Ref<SqlStorage> getSql(jsg::Lock& js);

  jsg::Ref<SyncKvStorage> getKv(jsg::Lock& js);

  // Get a bookmark for the current state of the database. Note that since this is async, the
  // bookmark will include any writes in the current atomic batch, including writes that are
  // performed after this call begins. It could also include concurrent writes that haven't happened
  // yet, unless blockConcurrencyWhile() is used to prevent them.
  kj::Promise<kj::String> getCurrentBookmark();

  // Get a bookmark representing approximately the given timestamp, which is a time up to 30 days
  // in the past (or whatever the backup retention period is).
  kj::Promise<kj::String> getBookmarkForTime(kj::Date timestamp);

  // Arrange that the next time the Durable Object restarts, the database will be restored to
  // the state represented by the given bookmark. This returns a bookmark string which represents
  // the state immediately before the restoration takes place, and thus can be used to undo the
  // restore. (This bookmark technically refers to a *future* state -- it specifies the state the
  // object will have at the end of the current session.)
  //
  // It is up to the caller to force a restart in order to complete the restoration, for instance
  // by calling state.abort() or by throwing from a blockConcurrencyWhile() callback.
  kj::Promise<kj::String> onNextSessionRestoreBookmark(kj::String bookmark);

  // Wait until the database has been updated to the state represented by `bookmark`.
  //
  // `waitForBookmark` is useful synchronizing requests across replicas of the same database.  On
  // primary databases, `waitForBookmark` will resolve immediately.  On replica databases,
  // `waitForBookmark` will resolve when the replica has been updated to a point at or after
  // `bookmark`.
  kj::Promise<void> waitForBookmark(kj::String bookmark);

  // Arrange to create replicas for this Durable Object.
  //
  // Once a Durable Object instance calls `ensureReplicas`, all subsequent calls will be no-ops,
  // making it idempotent, unless `disableReplicas` has been called between `ensureReplicas` calls.
  void ensureReplicas();

  // Arrange to disable replicas for this Durable Object.
  //
  // If replicas have never been created, this is a no-op. Similar to `ensureReplicas`, repeated
  // calls are no-ops unless `ensureReplicas` re-enabled the replicas.
  void disableReplicas();

  jsg::Optional<jsg::Ref<DurableObject>> getPrimary(jsg::Lock& js);

  JSG_RESOURCE_TYPE(DurableObjectStorage, CompatibilityFlags::Reader flags) {
    JSG_METHOD(get);
    JSG_METHOD(list);
    JSG_METHOD(put);
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(deleteAll);
    JSG_METHOD(transaction);
    JSG_METHOD(getAlarm);
    JSG_METHOD(setAlarm);
    JSG_METHOD(deleteAlarm);
    JSG_METHOD(sync);

    JSG_LAZY_INSTANCE_PROPERTY(sql, getSql);
    JSG_LAZY_INSTANCE_PROPERTY(kv, getKv);
    JSG_METHOD(transactionSync);

    JSG_METHOD(getCurrentBookmark);
    JSG_METHOD(getBookmarkForTime);
    JSG_METHOD(onNextSessionRestoreBookmark);

    if (flags.getWorkerdExperimental()) {
      JSG_METHOD(waitForBookmark);
      JSG_READONLY_INSTANCE_PROPERTY(primary, getPrimary);
    }

    if (flags.getReplicaRouting()) {
      JSG_METHOD(ensureReplicas);
      JSG_METHOD(disableReplicas);
    }

    JSG_TS_OVERRIDE({
      get<T = unknown>(key: string, options?: DurableObjectGetOptions): Promise<T | undefined>;
      get<T = unknown>(keys: string[], options?: DurableObjectGetOptions): Promise<Map<string, T>>;

      list<T = unknown>(options?: DurableObjectListOptions): Promise<Map<string, T>>;

      put<T>(key: string, value: T, options?: DurableObjectPutOptions): Promise<void>;
      put<T>(entries: Record<string, T>, options?: DurableObjectPutOptions): Promise<void>;

      delete(key: string, options?: DurableObjectPutOptions): Promise<boolean>;
      delete(keys: string[], options?: DurableObjectPutOptions): Promise<number>;

      transaction<T>(closure: (txn: DurableObjectTransaction) => Promise<T>): Promise<T>;
      transactionSync<T>(closure: () => T): T;
    });
  }

 protected:
  ActorCacheOps& getCache(kj::StringPtr op) override;

  bool useDirectIo() override {
    return false;
  }

 private:
  IoPtr<ActorCacheInterface> cache;
  bool enableSql;
  uint transactionSyncDepth = 0;

  // Set if this is a replica Durable Object.
  kj::Maybe<jsg::Ref<DurableObject>> maybePrimary;
};

class DurableObjectTransaction final: public jsg::Object, public DurableObjectStorageOperations {
 public:
  DurableObjectTransaction(IoOwn<ActorCacheInterface::Transaction> cacheTxn)
      : cacheTxn(kj::mv(cacheTxn)) {}

  // Called from C++, not JS, after the transaction callback has completed (successfully or not).
  // These methods do nothing if the transaction is already committed / rolled back.
  kj::Promise<void> maybeCommit();

  // Called from C++, not JS, after the transaction callback has completed (successfully or not).
  // These methods do nothing if the transaction is already committed / rolled back.
  void maybeRollback();

  void rollback();  // called from JS

  // Just throws an exception saying this isn't supported.
  void deleteAll();

  JSG_RESOURCE_TYPE(DurableObjectTransaction) {
    JSG_METHOD(get);
    JSG_METHOD(list);
    JSG_METHOD(put);
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(deleteAll);
    JSG_METHOD(rollback);
    JSG_METHOD(getAlarm);
    JSG_METHOD(setAlarm);
    JSG_METHOD(deleteAlarm);

    JSG_TS_OVERRIDE({
      get<T = unknown>(key: string, options?: DurableObjectGetOptions): Promise<T | undefined>;
      get<T = unknown>(keys: string[], options?: DurableObjectGetOptions): Promise<Map<string, T>>;

      list<T = unknown>(options?: DurableObjectListOptions): Promise<Map<string, T>>;

      put<T>(key: string, value: T, options?: DurableObjectPutOptions): Promise<void>;
      put<T>(entries: Record<string, T>, options?: DurableObjectPutOptions): Promise<void>;

      delete(key: string, options?: DurableObjectPutOptions): Promise<boolean>;
      delete(keys: string[], options?: DurableObjectPutOptions): Promise<number>;

      deleteAll: never;
    });
  }

 protected:
  ActorCacheOps& getCache(kj::StringPtr op) override;

  bool useDirectIo() override {
    return false;
  }

 private:
  // Becomes null when committed or rolled back.
  kj::Maybe<IoOwn<ActorCacheInterface::Transaction>> cacheTxn;

  bool rolledBack = false;

  friend DurableObjectStorage;
};

class DurableObjectFacets: public jsg::Object {
 public:
  DurableObjectFacets(kj::Maybe<IoPtr<Worker::Actor::FacetManager>> facetManager)
      : facetManager(kj::mv(facetManager)) {}

  // Describes how to run a facet. The app provides this when first accessing a facet that isn't
  // already running.
  struct StartupOptions {
    // The actor class to use to implement the facet.
    //
    // Note that the $ is needed only because `class` is a keyword in C++. JSG removes the $ from
    // the name in the JS API. C++ does not officially recognize the existence of a $ symbol but
    // all major compilers support using it as if it were a letter.
    kj::OneOf<jsg::Ref<DurableObjectClass>,
        jsg::Ref<LoopbackDurableObjectNamespace>,
        jsg::Ref<LoopbackColoLocalActorNamespace>>
        $class;

    // Value to expose as `ctx.id` in the facet.
    jsg::Optional<kj::OneOf<jsg::Ref<DurableObjectId>, kj::String>> id;

    JSG_STRUCT($class, id);

    JSG_STRUCT_TS_OVERRIDE(FacetStartupOptions<
        T extends Rpc.DurableObjectBranded | undefined = undefined> {
      class: DurableObjectClass<T>;
      id?: DurableObjectId | string;

      $class: never;  // work around generate-types bug
    });
  };

  // Get a facet by name, starting it if it isn't already running. `getStartupOptions` is invoked
  // only if the facet wasn't already running, to get information needed to start the facet.
  //
  // Returns a `Fetcher` instead of a `DurableObject` becasue the returend stub does not have the
  // `id` or `name` methods that a DO stub normally has.
  jsg::Ref<Fetcher> get(jsg::Lock& js,
      kj::String name,
      jsg::Function<jsg::Promise<StartupOptions>()> getStartupOptions);

  void abort(jsg::Lock& js, kj::String name, jsg::JsValue reason);
  void delete_(jsg::Lock& js, kj::String name);

  JSG_RESOURCE_TYPE(DurableObjectFacets) {
    JSG_METHOD(get);
    JSG_METHOD(abort);
    JSG_METHOD_NAMED(delete, delete_);

    JSG_TS_OVERRIDE({
      get<T extends Rpc.DurableObjectBranded | undefined = undefined>(
          name: string,
          getStartupOptions: () => FacetStartupOptions<T> | Promise<FacetStartupOptions<T>>)
          : Fetcher<T>;
    });
  }

 private:
  kj::Maybe<IoPtr<Worker::Actor::FacetManager>> facetManager;

  Worker::Actor::FacetManager& getFacetManager() {
    return *JSG_REQUIRE_NONNULL(
        facetManager, Error, "This Durable Object does not support creating facets.");
  }
};

// The type placed in event.actorState (pre-modules API).
// NOTE: It hasn't been renamed under the assumption that it will only be
// used for colo-local namespaces.
class ActorState: public jsg::Object {
  // TODO(cleanup): Remove getPersistent method that isn't supported for colo-local actors anymore.
 public:
  ActorState(Worker::Actor::Id actorId,
      kj::Maybe<jsg::JsRef<jsg::JsValue>> transient,
      kj::Maybe<jsg::Ref<DurableObjectStorage>> persistent);

  kj::OneOf<jsg::Ref<DurableObjectId>, kj::StringPtr> getId(jsg::Lock& js);

  jsg::Optional<jsg::JsValue> getTransient(jsg::Lock& js) {
    return transient.map([&](jsg::JsRef<jsg::JsValue>& v) { return v.getHandle(js); });
  }

  jsg::Optional<jsg::Ref<DurableObjectStorage>> getPersistent() {
    return persistent.map([&](jsg::Ref<DurableObjectStorage>& p) { return p.addRef(); });
  }

  JSG_RESOURCE_TYPE(ActorState) {
    JSG_READONLY_INSTANCE_PROPERTY(id, getId);
    JSG_READONLY_INSTANCE_PROPERTY(transient, getTransient);
    JSG_READONLY_INSTANCE_PROPERTY(persistent, getPersistent);

    JSG_TS_OVERRIDE(type ActorState = never);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    KJ_SWITCH_ONEOF(id) {
      KJ_CASE_ONEOF(str, kj::String) {
        tracker.trackField("id", str);
      }
      KJ_CASE_ONEOF(id, kj::Own<ActorIdFactory::ActorId>) {
        // TODO(later): This only yields the shallow size of the ActorId and not the
        // size of the actual value. Should probably make ActorID a MemoryRetainer.
        tracker.trackFieldWithSize("id", sizeof(ActorIdFactory::ActorId));
      }
    }
    tracker.trackField("transient", transient);
    tracker.trackField("persistent", persistent);
  }

 private:
  Worker::Actor::Id id;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> transient;
  kj::Maybe<jsg::Ref<DurableObjectStorage>> persistent;
};

class WebSocketRequestResponsePair: public jsg::Object {
 public:
  WebSocketRequestResponsePair(kj::String request, kj::String response)
      : request(kj::mv(request)),
        response(kj::mv(response)) {};

  static jsg::Ref<WebSocketRequestResponsePair> constructor(
      jsg::Lock& js, kj::String request, kj::String response) {
    return js.alloc<WebSocketRequestResponsePair>(kj::mv(request), kj::mv(response));
  };

  kj::StringPtr getRequest() {
    return request.asPtr();
  }
  kj::StringPtr getResponse() {
    return response.asPtr();
  }

  JSG_RESOURCE_TYPE(WebSocketRequestResponsePair) {
    JSG_READONLY_PROTOTYPE_PROPERTY(request, getRequest);
    JSG_READONLY_PROTOTYPE_PROPERTY(response, getResponse);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("request", request);
    tracker.trackField("response", response);
  }

 private:
  kj::String request;
  kj::String response;
};

// The type passed as the first parameter to durable object class's constructor.
class DurableObjectState: public jsg::Object {
 public:
  DurableObjectState(jsg::Lock& js,
      Worker::Actor::Id actorId,
      jsg::JsValue exports,
      jsg::JsValue props,
      kj::Maybe<jsg::Ref<DurableObjectStorage>> storage,
      kj::Maybe<rpc::Container::Client> container,
      bool containerRunning,
      kj::Maybe<Worker::Actor::FacetManager&> facetManager,
      kj::Maybe<ActorVersion> version = kj::none);

  void waitUntil(kj::Promise<void> promise);

  jsg::JsValue getExports(jsg::Lock& js) {
    return exports.getHandle(js);
  }

  jsg::JsValue getProps(jsg::Lock& js) {
    return props.getHandle(js);
  }

  kj::OneOf<jsg::Ref<DurableObjectId>, kj::StringPtr> getId(jsg::Lock& js);

  jsg::Optional<jsg::Ref<DurableObjectStorage>> getStorage() {
    return storage.map([&](jsg::Ref<DurableObjectStorage>& p) { return p.addRef(); });
  }

  struct Version {
    jsg::Optional<kj::StringPtr> cohort;
    JSG_STRUCT(cohort);
  };
  jsg::Optional<Version> getVersion() {
    return version.map([](ActorVersion& v) -> Version {
      return Version{.cohort = v.cohort.map([](kj::String& s) -> kj::StringPtr { return s; })};
    });
  }
  jsg::Optional<jsg::Ref<Container>> getContainer() {
    return container.map([](jsg::Ref<Container>& c) { return c.addRef(); });
  }

  jsg::Ref<DurableObjectFacets> getFacets(jsg::Lock& js) {
    return js.alloc<DurableObjectFacets>(facetManager);
  }

  jsg::Promise<jsg::JsRef<jsg::JsValue>> blockConcurrencyWhile(
      jsg::Lock& js, jsg::Function<jsg::Promise<jsg::JsRef<jsg::JsValue>>()> callback);

  // Reset the object, including breaking the output gate and canceling any writes that haven't
  // been committed yet.
  void abort(jsg::Lock& js, jsg::Optional<kj::String> reason);

  // Sets and returns a new hibernation manager in an actor if there's none or returns the existing.
  Worker::Actor::HibernationManager& maybeInitHibernationManager(Worker::Actor& actor);

  // Adds a WebSocket to the set attached to this object.
  // `ws.accept()` must NOT have been called separately.
  // Once called, any incoming messages will be delivered
  // by calling the Durable Object's webSocketMessage()
  // handler, and webSocketClose() will be invoked upon
  // disconnect.
  //
  // After calling this, the WebSocket is accepted, so
  // its send() and close() methods can be used to send
  // messages. It should be noted that calling addEventListener()
  // on the websocket does nothing, since inbound events will
  // automatically be delivered to one of the webSocketMessage()/
  // webSocketClose()/webSocketError() handlers. No inbound events
  // to a WebSocket accepted via acceptWebSocket() will ever be
  // delivered to addEventListener(), so there is no reason to call it.
  //
  // `tags` are string tags which can be used to look up
  // the WebSocket with getWebSockets().
  void acceptWebSocket(jsg::Ref<WebSocket> ws, jsg::Optional<kj::Array<kj::String>> tags);

  // Gets an array of accepted WebSockets matching the given tag.
  // If no tag is provided, an array of all accepted WebSockets is returned.
  // Disconnected WebSockets are automatically removed from the list.
  kj::Array<jsg::Ref<api::WebSocket>> getWebSockets(jsg::Lock& js, jsg::Optional<kj::String> tag);

  // Sets an object-wide websocket auto response message for a specific
  // request string. All websockets belonging to the same object must
  // reply to the request with the matching response, then store the timestamp at which
  // the request was received.
  // If maybeReqResp is not set, we consider it as unset and remove any set request response pair.
  void setWebSocketAutoResponse(
      jsg::Optional<jsg::Ref<api::WebSocketRequestResponsePair>> maybeReqResp);

  // Gets the currently set object-wide websocket auto response.
  kj::Maybe<jsg::Ref<api::WebSocketRequestResponsePair>> getWebSocketAutoResponse(jsg::Lock& js);

  // Get the last auto response timestamp or null
  kj::Maybe<kj::Date> getWebSocketAutoResponseTimestamp(jsg::Ref<WebSocket> ws);

  // Sets or unsets the timeout for hibernatable websocket events, preventing the execution of
  // the event from taking longer than the specified timeout, if set.
  void setHibernatableWebSocketEventTimeout(jsg::Optional<uint32_t> timeoutMs);

  // Get the currently set hibernatable websocket event timeout if set, or kj::none if not.
  kj::Maybe<uint32_t> getHibernatableWebSocketEventTimeout();

  // Gets an array of tags that this websocket was accepted with. If the given websocket is not
  // hibernatable, we'll throw an error because regular websockets do not have tags.
  kj::Array<kj::StringPtr> getTags(jsg::Lock& js, jsg::Ref<api::WebSocket> ws);

  JSG_RESOURCE_TYPE(DurableObjectState, CompatibilityFlags::Reader flags) {
    JSG_METHOD(waitUntil);
    if (flags.getEnableCtxExports()) {
      JSG_LAZY_INSTANCE_PROPERTY(exports, getExports);
    }
    JSG_LAZY_INSTANCE_PROPERTY(props, getProps);
    JSG_LAZY_INSTANCE_PROPERTY(id, getId);
    JSG_LAZY_INSTANCE_PROPERTY(storage, getStorage);
    JSG_LAZY_INSTANCE_PROPERTY(container, getContainer);
    if (flags.getWorkerdExperimental()) {
      // Experimental new API, details may change!
      JSG_LAZY_INSTANCE_PROPERTY(facets, getFacets);
      JSG_LAZY_INSTANCE_PROPERTY(version, getVersion);
    }
    JSG_METHOD(blockConcurrencyWhile);
    JSG_METHOD(acceptWebSocket);
    JSG_METHOD(getWebSockets);
    JSG_METHOD(setWebSocketAutoResponse);
    JSG_METHOD(getWebSocketAutoResponse);
    JSG_METHOD(getWebSocketAutoResponseTimestamp);
    JSG_METHOD(setHibernatableWebSocketEventTimeout);
    JSG_METHOD(getHibernatableWebSocketEventTimeout);
    JSG_METHOD(getTags);

    JSG_METHOD(abort);

    JSG_TS_ROOT();

    // Type overrides:
    // * Define Props/Exports type parameters.
    // * Make `storage` non-optional
    // * Make `id` strictly `DurableObjectId` (it's only a string for colo-local actors which are
    //   not available publicly).
    if (flags.getEnableCtxExports()) {
      JSG_TS_OVERRIDE(<Props = unknown> {
        readonly props: Props;
        readonly exports: Cloudflare.Exports;
        readonly id: DurableObjectId;
        readonly storage: DurableObjectStorage;
        blockConcurrencyWhile<T>(callback: () => Promise<T>): Promise<T>;
      });
    } else {
      // No ctx.exports yet.
      JSG_TS_OVERRIDE(<Props = unknown> {
        readonly props: Props;
        readonly id: DurableObjectId;
        readonly storage: DurableObjectStorage;
        blockConcurrencyWhile<T>(callback: () => Promise<T>): Promise<T>;
      });
    }
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    KJ_SWITCH_ONEOF(id) {
      KJ_CASE_ONEOF(str, kj::String) {
        tracker.trackField("id", str);
      }
      KJ_CASE_ONEOF(id, kj::Own<ActorIdFactory::ActorId>) {
        // TODO(later): This only yields the shallow size of the ActorId and not the
        // size of the actual value. Should probably make ActorID a MemoryRetainer.
        tracker.trackFieldWithSize("id", sizeof(ActorIdFactory::ActorId));
      }
    }
    tracker.trackField("storage", storage);
  }

 private:
  Worker::Actor::Id id;
  jsg::JsRef<jsg::JsValue> exports;
  jsg::JsRef<jsg::JsValue> props;
  kj::Maybe<jsg::Ref<DurableObjectStorage>> storage;
  kj::Maybe<jsg::Ref<Container>> container;
  kj::Maybe<IoPtr<Worker::Actor::FacetManager>> facetManager;
  kj::Maybe<ActorVersion> version;

  // Limits for Hibernatable WebSocket tags.

  const size_t MAX_TAGS_PER_CONNECTION = 10;
  const size_t MAX_TAG_LENGTH = 256;
};

#define EW_ACTOR_STATE_ISOLATE_TYPES                                                               \
  api::ActorState, api::DurableObjectState, api::DurableObjectTransaction,                         \
      api::DurableObjectStorage, api::DurableObjectStorage::TransactionOptions,                    \
      api::DurableObjectStorageOperations::ListOptions,                                            \
      api::DurableObjectStorageOperations::GetOptions,                                             \
      api::DurableObjectStorageOperations::GetAlarmOptions,                                        \
      api::DurableObjectStorageOperations::PutOptions,                                             \
      api::DurableObjectStorageOperations::SetAlarmOptions, api::WebSocketRequestResponsePair,     \
      api::DurableObjectFacets, api::DurableObjectFacets::StartupOptions,                          \
      api::DurableObjectState::Version

}  // namespace workerd::api
