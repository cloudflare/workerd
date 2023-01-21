// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// APIs that an Actor (Durable Object) uses to access its own state.
//
// See actor.h for APIs used by other Workers to talk to Actors.

#include <workerd/jsg/jsg.h>
#include <workerd/io/io-context.h>
#include <workerd/io/actor-storage.capnp.h>
#include <kj/async.h>
#include <v8.h>
#include <workerd/io/promise-wrapper.h>
#include "util.h"
#include <workerd/io/actor-cache.h>
#include <workerd/io/actor-sqlite.h>

namespace workerd::api {

// Forward-declared to avoid dependency cycle (actor.h -> http.h -> basics.h -> actor-state.h)
class DurableObjectId;

kj::Array<kj::byte> serializeV8Value(v8::Local<v8::Value> value, v8::Isolate* isolate);

v8::Local<v8::Value> deserializeV8Value(
    kj::ArrayPtr<const char> key, kj::ArrayPtr<const kj::byte> buf, v8::Isolate* isolate);

class DurableObjectStorageOperations {
  // Common implementation of DurableObjectStorage and DurableObjectTransaction. This class is
  // designed to be used as a mixin.

public:
  struct GetOptions {
    jsg::Optional<bool> allowConcurrency;
    jsg::Optional<bool> noCache;

    inline operator ActorCache::ReadOptions() const {
      return {
        .noCache = noCache.orDefault(false)
      };
    }

    JSG_STRUCT(allowConcurrency, noCache);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectGetOptions); // Rename from DurableObjectStorageOperationsGetOptions
  };

  jsg::Promise<jsg::Value> get(
      kj::OneOf<kj::String, kj::Array<kj::String>> keys, jsg::Optional<GetOptions> options,
      v8::Isolate* isolate);

  struct GetAlarmOptions {
    jsg::Optional<bool> allowConcurrency;

    JSG_STRUCT(allowConcurrency);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectGetAlarmOptions); // Rename from DurableObjectStorageOperationsGetAlarmOptions
  };

  jsg::Promise<kj::Maybe<double>> getAlarm(jsg::Optional<GetAlarmOptions> options, v8::Isolate* isolate);

  struct ListOptions {
    jsg::Optional<kj::String> start;
    jsg::Optional<kj::String> startAfter;
    jsg::Optional<kj::String> end;
    jsg::Optional<kj::String> prefix;
    jsg::Optional<bool> reverse;
    jsg::Optional<int> limit;

    jsg::Optional<bool> allowConcurrency;
    jsg::Optional<bool> noCache;

    inline operator ActorCache::ReadOptions() const {
      return {
        .noCache = noCache.orDefault(false)
      };
    }

    JSG_STRUCT(start, startAfter, end, prefix, reverse, limit, allowConcurrency, noCache);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectListOptions); // Rename from DurableObjectStorageOperationsListOptions
  };

  jsg::Promise<jsg::Value> list(jsg::Optional<ListOptions> options, v8::Isolate* isolate);

  struct PutOptions {
    jsg::Optional<bool> allowConcurrency;
    jsg::Optional<bool> allowUnconfirmed;
    jsg::Optional<bool> noCache;

    inline operator ActorCache::WriteOptions() const {
      return {
        .allowUnconfirmed = allowUnconfirmed.orDefault(false),
        .noCache = noCache.orDefault(false)
      };
    }

    JSG_STRUCT(allowConcurrency, allowUnconfirmed, noCache);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectPutOptions); // Rename from DurableObjectStorageOperationsPutOptions
  };

  jsg::Promise<void> put(jsg::Lock& js,
      kj::OneOf<kj::String, jsg::Dict<v8::Local<v8::Value>>> keyOrEntries,
      jsg::Optional<v8::Local<v8::Value>> value, jsg::Optional<PutOptions> options,
      v8::Isolate* isolate, const jsg::TypeHandler<PutOptions>& optionsTypeHandler);

  kj::OneOf<jsg::Promise<bool>, jsg::Promise<int>> delete_(
      kj::OneOf<kj::String, kj::Array<kj::String>> keys, jsg::Optional<PutOptions> options,
      v8::Isolate* isolate);

  struct SetAlarmOptions {
    jsg::Optional<bool> allowConcurrency;
    jsg::Optional<bool> allowUnconfirmed;
    // We don't allow noCache for alarm puts.

    inline operator ActorCache::WriteOptions() const {
      return {
        .allowUnconfirmed = allowUnconfirmed.orDefault(false),
      };
    }

    JSG_STRUCT(allowConcurrency, allowUnconfirmed);
    JSG_STRUCT_TS_OVERRIDE(DurableObjectSetAlarmOptions); // Rename from DurableObjectStorageOperationsSetAlarmOptions
  };

  jsg::Promise<void> setAlarm(kj::Date scheduledTime, jsg::Optional<SetAlarmOptions> options,
      v8::Isolate* isolate);
  jsg::Promise<void> deleteAlarm(jsg::Optional<SetAlarmOptions> options, v8::Isolate* isolate);

protected:
  typedef kj::StringPtr OpName;
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

  virtual ActorCacheInterface& getCache(OpName op) = 0;

  virtual bool useDirectIo() = 0;
  // Whether to skip caching and allow concurrency on all operations.

  template <typename T>
  T configureOptions(T&& options) {
    // Method that should be called at the start of each storage operation to override any of the
    // options as appropriate.
    if (useDirectIo()) {
      options.allowConcurrency = true;
      options.noCache = true;
    }
    return kj::mv(options);
  }

private:
  jsg::Promise<jsg::Value> getOne(kj::String key, const GetOptions& options, v8::Isolate* isolate);
  jsg::Promise<jsg::Value> getMultiple(kj::Array<kj::String> keys, const GetOptions& options,
                                         v8::Isolate* isolate);

  jsg::Promise<void> putOne(kj::String key, v8::Local<v8::Value> value, const PutOptions& options,
                             v8::Isolate* isolate);
  jsg::Promise<void> putMultiple(jsg::Dict<v8::Local<v8::Value>> entries,
                                  const PutOptions& options, v8::Isolate* isolate);

  jsg::Promise<bool> deleteOne(kj::String key, const PutOptions& options, v8::Isolate* isolate);
  jsg::Promise<int> deleteMultiple(kj::Array<kj::String> keys, const PutOptions& options,
                                    v8::Isolate* isolate);
};

class DurableObjectTransaction;

class DurableObjectStorage: public jsg::Object, public DurableObjectStorageOperations {
public:
  DurableObjectStorage(IoPtr<ActorCache> cache)
    : cache(kj::mv(cache)) {}
  DurableObjectStorage(IoPtr<ActorSqlite> sqliteKv)
    : cache(kj::mv(sqliteKv)) {}

  struct TransactionOptions {
    jsg::Optional<kj::Date> asOfTime;
    jsg::Optional<bool> lowPriority;

    JSG_STRUCT(asOfTime, lowPriority);
    JSG_STRUCT_TS_OVERRIDE(type TransactionOptions = never);
    // Omit from definitions
  };

  jsg::Promise<jsg::Value> transaction(jsg::Lock& js,
      jsg::Function<jsg::Promise<jsg::Value>(jsg::Ref<DurableObjectTransaction>)> closure,
      jsg::Optional<TransactionOptions> options);

  jsg::Promise<void> deleteAll(jsg::Lock& js, jsg::Optional<PutOptions> options);

  jsg::Promise<void> sync(jsg::Lock& js);

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

    JSG_TS_OVERRIDE({
      get<T = unknown>(key: string, options?: DurableObjectGetOptions): Promise<T | undefined>;
      get<T = unknown>(keys: string[], options?: DurableObjectGetOptions): Promise<Map<string, T>>;

      list<T = unknown>(options?: DurableObjectListOptions): Promise<Map<string, T>>;

      put<T>(key: string, value: T, options?: DurableObjectPutOptions): Promise<void>;
      put<T>(entries: Record<string, T>, options?: DurableObjectPutOptions): Promise<void>;

      delete(key: string, options?: DurableObjectPutOptions): Promise<boolean>;
      delete(keys: string[], options?: DurableObjectPutOptions): Promise<number>;

      transaction<T>(closure: (txn: DurableObjectTransaction) => Promise<T>): Promise<T>;
    });
  }

protected:
  ActorCacheInterface& getCache(kj::StringPtr op) override;

  bool useDirectIo() override {
    return false;
  }

private:
  kj::OneOf<IoPtr<ActorCache>, IoPtr<ActorSqlite>> cache;
};

class DurableObjectTransaction final: public jsg::Object, public DurableObjectStorageOperations {
public:
  DurableObjectTransaction(IoOwn<ActorCache::Transaction> cacheTxn)
    : cacheTxn(kj::mv(cacheTxn)) {}

  kj::Promise<void> maybeCommit();
  void maybeRollback();
  // Called from C++, not JS, after the transaction callback has completed (successfully or not).
  // These methods do nothing if the transaction is already committed / rolled back.

  void rollback();  // called from JS

  void deleteAll();
  // Just throws an exception saying this isn't supported.

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
  ActorCacheInterface& getCache(kj::StringPtr op) override;

  bool useDirectIo() override {
    return false;
  }

private:
  kj::Maybe<IoOwn<ActorCache::Transaction>> cacheTxn;
  // Becomes null when committed or rolled back.

  bool rolledBack = false;

  friend DurableObjectStorage;
};

class ActorState: public jsg::Object {
  // The type placed in event.actorState (pre-modules API).
  // NOTE: It hasn't been renamed under the assumption that it will only be
  // used for colo-local namespaces.
  // TODO(cleanup): Remove getPersistent method that isn't supported for colo-local actors anymore.

public:
  ActorState(Worker::Actor::Id actorId, kj::Maybe<jsg::Value> transient,
      kj::Maybe<jsg::Ref<DurableObjectStorage>> persistent);

  kj::OneOf<jsg::Ref<DurableObjectId>, kj::StringPtr> getId();

  jsg::Optional<v8::Local<v8::Value>> getTransient(v8::Isolate* isolate) {
    return transient.map([&](jsg::Value& v) { return v.getHandle(isolate); });
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

private:
  Worker::Actor::Id id;
  kj::Maybe<jsg::Value> transient;
  kj::Maybe<jsg::Ref<DurableObjectStorage>> persistent;
};

class DurableObjectState: public jsg::Object {
  // The type passed as the first parameter to durable object class's constructor.

public:
  DurableObjectState(Worker::Actor::Id actorId,
      kj::Maybe<jsg::Ref<DurableObjectStorage>> storage);

  void waitUntil(kj::Promise<void> promise);

  kj::OneOf<jsg::Ref<DurableObjectId>, kj::StringPtr> getId();

  jsg::Optional<jsg::Ref<DurableObjectStorage>> getStorage() {
    return storage.map([&](jsg::Ref<DurableObjectStorage>& p) { return p.addRef(); });
  }

  jsg::Promise<jsg::Value> blockConcurrencyWhile(jsg::Lock& js,
      jsg::Function<jsg::Promise<jsg::Value>()> callback);

  JSG_RESOURCE_TYPE(DurableObjectState) {
    JSG_METHOD(waitUntil);
    JSG_READONLY_INSTANCE_PROPERTY(id, getId);
    JSG_READONLY_INSTANCE_PROPERTY(storage, getStorage);
    JSG_METHOD(blockConcurrencyWhile);

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE({
      readonly id: DurableObjectId;
      readonly storage: DurableObjectStorage;
      blockConcurrencyWhile<T>(callback: () => Promise<T>): Promise<T>;
    });
    // Make `storage` non-optional
  }

private:
  Worker::Actor::Id id;
  kj::Maybe<jsg::Ref<DurableObjectStorage>> storage;
};

#define EW_ACTOR_STATE_ISOLATE_TYPES                     \
  api::ActorState,                                       \
  api::DurableObjectState,                               \
  api::DurableObjectTransaction,                         \
  api::DurableObjectStorage,                             \
  api::DurableObjectStorage::TransactionOptions,         \
  api::DurableObjectStorageOperations::ListOptions,      \
  api::DurableObjectStorageOperations::GetOptions,       \
  api::DurableObjectStorageOperations::GetAlarmOptions,  \
  api::DurableObjectStorageOperations::PutOptions,       \
  api::DurableObjectStorageOperations::SetAlarmOptions

}  // namespace workerd::api
