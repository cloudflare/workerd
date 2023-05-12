// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-state.h"
#include "actor.h"
#include "util.h"
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>
#include <workerd/jsg/util.h>
#include <v8.h>
#include <workerd/io/actor-cache.h>
#include <workerd/io/actor-storage.h>
#include <workerd/io/actor-sqlite.h>
#include "sql.h"
#include <workerd/api/web-socket.h>
#include <workerd/io/hibernation-manager.h>

namespace workerd::api {

namespace {

constexpr size_t BILLING_UNIT = 4096;

enum class BillAtLeastOne {
  NO, YES
};

uint32_t billingUnits(size_t bytes, BillAtLeastOne billAtLeastOne = BillAtLeastOne::YES) {
  if (billAtLeastOne == BillAtLeastOne::YES && bytes == 0) {
    return 1; // always bill for at least 1 billing unit
  }
  return bytes / BILLING_UNIT + (bytes % BILLING_UNIT != 0);
}

v8::Local<v8::Value> deserializeMaybeV8Value(
    kj::ArrayPtr<const char> key, kj::Maybe<kj::ArrayPtr<const kj::byte>> buf,
    v8::Isolate* isolate) {
  KJ_IF_MAYBE(b, buf) {
    return deserializeV8Value(key, *b, isolate);
  } else {
    return v8::Undefined(isolate);
  }
}

template <typename T, typename Options, typename Func>
auto transformCacheResult(
    v8::Isolate* isolate, kj::OneOf<T, kj::Promise<T>> input, const Options& options, Func&& func)
    -> jsg::Promise<decltype(func(isolate, kj::instance<T>()))> {
  KJ_SWITCH_ONEOF(input) {
    KJ_CASE_ONEOF(value, T) {
      return jsg::resolvedPromise(isolate, func(isolate, kj::mv(value)));
    }
    KJ_CASE_ONEOF(promise, kj::Promise<T>) {
      auto& context = IoContext::current();
      if (options.allowConcurrency.orDefault(false)) {
        return context.awaitIo(kj::mv(promise),
            [func = kj::fwd<Func>(func), isolate](T&& value) mutable {
          return func(isolate, kj::mv(value));
        });
      } else {
        return context.awaitIoWithInputLock(kj::mv(promise),
            [func = kj::fwd<Func>(func), isolate](T&& value) mutable {
          return func(isolate, kj::mv(value));
        });
      }
    }
  }
  KJ_UNREACHABLE;
}

template <typename T, typename Options, typename Func>
auto transformCacheResultWithCacheStatus(
    v8::Isolate* isolate, kj::OneOf<T, kj::Promise<T>> input, const Options& options, Func&& func)
    -> jsg::Promise<decltype(func(isolate, kj::instance<T>(), kj::instance<bool>()))> {
  KJ_SWITCH_ONEOF(input) {
    KJ_CASE_ONEOF(value, T) {
      return jsg::resolvedPromise(isolate, func(isolate, kj::mv(value), true));
    }
    KJ_CASE_ONEOF(promise, kj::Promise<T>) {
      auto& context = IoContext::current();
      if (options.allowConcurrency.orDefault(false)) {
        return context.awaitIo(kj::mv(promise),
            [func = kj::fwd<Func>(func), isolate](T&& value) mutable {
          return func(isolate, kj::mv(value), false);
        });
      } else {
        return context.awaitIoWithInputLock(kj::mv(promise),
            [func = kj::fwd<Func>(func), isolate](T&& value) mutable {
          return func(isolate, kj::mv(value), false);
        });
      }
    }
  }
  KJ_UNREACHABLE;
}

template <typename Options>
jsg::Promise<void> transformMaybeBackpressure(
    v8::Isolate* isolate, const Options& options,
    kj::Maybe<kj::Promise<void>> maybeBackpressure) {
  KJ_IF_MAYBE(backpressure, maybeBackpressure) {
    // Note: In practice `allowConcurrency` will have no effect on a backpressure promise since
    //   backpressure blocks everything anyway, but we pass the option through for consistency in
    //   case of future changes.
    auto& context = IoContext::current();
    if (options.allowConcurrency.orDefault(false)) {
      return context.awaitIo(kj::mv(*backpressure));
    } else {
      return context.awaitIoWithInputLock(kj::mv(*backpressure));
    }
  } else {
    return jsg::resolvedPromise(isolate);
  }
}

ActorObserver& currentActorMetrics() {
  return IoContext::current().getActorOrThrow().getMetrics();
}

}  // namespace

jsg::Promise<jsg::Value> DurableObjectStorageOperations::get(
    kj::OneOf<kj::String, kj::Array<kj::String>> keys, jsg::Optional<GetOptions> maybeOptions,
    v8::Isolate* isolate) {
  auto options = configureOptions(kj::mv(maybeOptions).orDefault(GetOptions{}));
  KJ_SWITCH_ONEOF(keys) {
    KJ_CASE_ONEOF(s, kj::String) {
      return getOne(kj::mv(s), options, isolate);
    }
    KJ_CASE_ONEOF(a, kj::Array<kj::String>) {
      return getMultiple(kj::mv(a), options, isolate);
    }
  }
  KJ_UNREACHABLE
}

jsg::Value listResultsToMap(v8::Isolate* isolate, ActorCacheOps::GetResultList value, bool completelyCached) {
  v8::HandleScope scope(isolate);
  auto context = isolate->GetCurrentContext();

  auto map = v8::Map::New(isolate);
  size_t cachedReadBytes = 0;
  size_t uncachedReadBytes = 0;
  for (auto entry: value) {
    auto& bytesRef = entry.status == ActorCacheOps::CacheStatus::CACHED
                   ? cachedReadBytes : uncachedReadBytes;
    bytesRef += entry.key.size() + entry.value.size();
    jsg::check(map->Set(context, jsg::v8Str(isolate, entry.key),
        deserializeV8Value(entry.key, entry.value, isolate)));
  }
  auto& actorMetrics = currentActorMetrics();
  if (cachedReadBytes || uncachedReadBytes) {
    size_t totalReadBytes = cachedReadBytes + uncachedReadBytes;
    uint32_t totalUnits = billingUnits(totalReadBytes);

    // If we went to disk, we want to ensure we bill at least 1 uncached unit.
    // Otherwise, we disable this behavior, to ensure a fully cached list will have
    // uncachedUnits == 0.
    auto billAtLeastOne = completelyCached ? BillAtLeastOne::NO : BillAtLeastOne::YES;
    uint32_t uncachedUnits = billingUnits(uncachedReadBytes, billAtLeastOne);
    uint32_t cachedUnits = totalUnits - uncachedUnits;

    actorMetrics.addUncachedStorageReadUnits(uncachedUnits);
    actorMetrics.addCachedStorageReadUnits(cachedUnits);
  } else {
    // We bill 1 uncached read unit if there was no results from the list.
    actorMetrics.addUncachedStorageReadUnits(1);
  }

  return jsg::Value(isolate, map);
}

kj::Function<jsg::Value(v8::Isolate*, ActorCacheOps::GetResultList)> getMultipleResultsToMap(
    size_t numInputKeys) {
  return [numInputKeys](v8::Isolate* isolate, ActorCacheOps::GetResultList value) mutable {
    v8::HandleScope scope(isolate);
    auto context = isolate->GetCurrentContext();

    auto map = v8::Map::New(isolate);
    uint32_t cachedUnits = 0;
    uint32_t uncachedUnits = 0;
    for (auto entry: value) {
      auto& unitsRef = entry.status == ActorCacheOps::CacheStatus::CACHED
                    ? cachedUnits : uncachedUnits;
      unitsRef += billingUnits(entry.key.size() + entry.value.size());
      jsg::check(map->Set(context, jsg::v8Str(isolate, entry.key),
          deserializeV8Value(entry.key, entry.value, isolate)));
    }
    auto& actorMetrics = currentActorMetrics();
    actorMetrics.addCachedStorageReadUnits(cachedUnits);

    size_t leftoverKeys = 0;
    if (numInputKeys >= value.size()) {
      leftoverKeys = numInputKeys - value.size();
    } else {
      KJ_LOG(ERROR, "More returned pairs than provided input keys in getMultipleResultsToMap",
          numInputKeys, value.size());
    }

    // leftover keys weren't in the result set, but potentially still
    // had to be queried for existence.
    //
    // TODO(someday): This isn't quite accurate -- we do cache negative entries.
    // Billing will still be correct today, but if we do ever start billing
    // only for uncached reads, we'll need to address this.
    actorMetrics.addUncachedStorageReadUnits(leftoverKeys + uncachedUnits);

    return jsg::Value(isolate, map);
  };
}

jsg::Promise<jsg::Value> DurableObjectStorageOperations::getOne(
    kj::String key, const GetOptions& options, v8::Isolate* isolate) {
  ActorStorageLimits::checkMaxKeySize(key);

  auto result = getCache(OP_GET).get(kj::str(key), options);
  return transformCacheResultWithCacheStatus(isolate, kj::mv(result), options,
      [key = kj::mv(key)](v8::Isolate* isolate, kj::Maybe<ActorCacheOps::Value> value, bool cached) {
    uint32_t units = 1;
    KJ_IF_MAYBE(v, value) {
      units = billingUnits(v->size());
    }
    auto& actorMetrics = currentActorMetrics();
    if (cached) {
      actorMetrics.addCachedStorageReadUnits(units);
    } else {
      actorMetrics.addUncachedStorageReadUnits(units);
    }
    return jsg::Value { isolate, deserializeMaybeV8Value(key, value, isolate) };
  });
}

jsg::Promise<kj::Maybe<double>> DurableObjectStorageOperations::getAlarm(
    jsg::Optional<GetAlarmOptions> maybeOptions, v8::Isolate* isolate) {
  // Even if we do not have an alarm handler, we might once have had one. It's fine to return
  // whatever a previous alarm setting or a falsy result.
  auto options = configureOptions(maybeOptions.map([](auto& o) {
    return GetOptions {
      .allowConcurrency = o.allowConcurrency,
      .noCache = false
    };
  }).orDefault(GetOptions{}));
  auto result = getCache(OP_GET_ALARM).getAlarm(options);

  return transformCacheResult(isolate, kj::mv(result), options,
      [](v8::Isolate* isolate, kj::Maybe<kj::Date> date) {
    return date.map([](auto& date) {
      return static_cast<double>((date - kj::UNIX_EPOCH) / kj::MILLISECONDS);
    });
  });
}

jsg::Promise<jsg::Value> DurableObjectStorageOperations::list(
    jsg::Optional<ListOptions> maybeOptions, v8::Isolate* isolate) {
  kj::String start;
  kj::Maybe<kj::String> end;
  bool reverse = false;
  kj::Maybe<uint> limit;

  auto makeEmptyResult = [&]() {
    return jsg::resolvedPromise(isolate, jsg::Value(isolate, v8::Map::New(isolate)));
  };

  KJ_IF_MAYBE(o, maybeOptions) {
    KJ_IF_MAYBE(s, o->start) {
      KJ_IF_MAYBE(sa, o->startAfter) {
        KJ_FAIL_REQUIRE("jsg.TypeError: list() cannot be called with both start and startAfter values.");
      }
      start = kj::mv(*s);
    }
    KJ_IF_MAYBE(sks, o->startAfter) {
      // Convert an exclusive startAfter into an inclusive start key here so that the implementation
      // doesn't need to handle both. This can be done simply by adding two NULL bytes. One to the end of
      // the startAfter and another to set the start key after startAfter.
      auto startAfterKey = kj::heapArray<char>(sks->size() + 2);

      // Copy over the original string.
      memcpy(startAfterKey.begin(), sks->begin(), sks->size());
      // Add one additional null byte to set the new start as the key immediately
      // after startAfter. This looks a little sketchy to be doing with strings rather
      // than arrays, but kj::String explicitly allows for NULL bytes inside of strings.
      startAfterKey[startAfterKey.size()-2] = '\0';
      // kj::String automatically reads the last NULL as string termination, so we need to add it twice
      // to make it stick in the final string.
      startAfterKey[startAfterKey.size()-1] = '\0';
      start = kj::String(kj::mv(startAfterKey));
    }
    KJ_IF_MAYBE(e, o->end) {
      end = kj::mv(*e);
    }
    KJ_IF_MAYBE(r, o->reverse) {
      reverse = *r;
    }
    KJ_IF_MAYBE(l, o->limit) {
      JSG_REQUIRE(*l > 0, TypeError, "List limit must be positive.");
      limit = *l;
    }
    KJ_IF_MAYBE(prefix, o->prefix) {
      // Let's clamp `start` and `end` to include only keys with the given prefix.
      if (prefix->size() > 0) {
        if (start < *prefix) {
          // `start` is before `prefix`, so listing should actually start at `prefix`.
          start = kj::str(*prefix);
        } else if (start.startsWith(*prefix)) {
          // `start` is within the prefix, so need not be modified.
        } else {
          // `start` comes after the last value with the prefix, so there's no overlap.
          return makeEmptyResult();
        }

        // Calculate the first key that sorts after all keys with the given prefix.
        kj::Vector<char> keyAfterPrefix(prefix->size());
        keyAfterPrefix.addAll(*prefix);
        while (!keyAfterPrefix.empty() && (byte)keyAfterPrefix.back() == 0xff) {
          keyAfterPrefix.removeLast();
        }
        if (keyAfterPrefix.empty()) {
          // The prefix is a string of some number of 0xff bytes, so includes the entire key space
          // up through the last possible key. Hence, there is no end. (But if an end was specified
          // earlier, that's still valid.)
        } else {
          keyAfterPrefix.back()++;
          keyAfterPrefix.add('\0');
          auto keyAfterPrefixStr = kj::String(keyAfterPrefix.releaseAsArray());

          KJ_IF_MAYBE(e, end) {
            if (*e <= *prefix) {
              // No keys could possibly match both the end and the prefix.
              return makeEmptyResult();
            } else if (e->startsWith(*prefix)) {
              // `end` is within the prefix, so need not be modified.
            } else {
              // `end` comes after all keys with the prefix, so we should stop at the end of the
              // prefix.
              end = kj::mv(keyAfterPrefixStr);
            }
          } else {
            // We didn't have any end set, so use the end of the prefix range.
            end = kj::mv(keyAfterPrefixStr);
          }
        }
      }
    }
  }

  KJ_IF_MAYBE(e, end) {
    if (*e <= start) {
      // Key range is empty.
      return makeEmptyResult();
    }
  }

  auto options = configureOptions(kj::mv(maybeOptions).orDefault(ListOptions{}));
  ActorCacheOps::ReadOptions readOptions = options;

  auto result = reverse
      ? getCache(OP_LIST).listReverse(kj::mv(start), kj::mv(end), limit, readOptions)
      : getCache(OP_LIST).list(kj::mv(start), kj::mv(end), limit, readOptions);
  return transformCacheResultWithCacheStatus(isolate, kj::mv(result), options, &listResultsToMap);
}

jsg::Promise<void> DurableObjectStorageOperations::put(jsg::Lock& js,
    kj::OneOf<kj::String, jsg::Dict<v8::Local<v8::Value>>> keyOrEntries,
    jsg::Optional<v8::Local<v8::Value>> value, jsg::Optional<PutOptions> maybeOptions,
    v8::Isolate* isolate, const jsg::TypeHandler<PutOptions>& optionsTypeHandler) {
  // TODO(soon): Add tests of data generated at current versions to ensure we'll
  // know before releasing any backwards-incompatible serializer changes,
  // potentially checking the header in addition to the value.
  auto options = configureOptions(kj::mv(maybeOptions).orDefault(PutOptions{}));
  KJ_SWITCH_ONEOF(keyOrEntries) {
    KJ_CASE_ONEOF(k, kj::String) {
      KJ_IF_MAYBE(v, value) {
        return putOne(kj::mv(k), *v, options, isolate);
      } else {
        JSG_FAIL_REQUIRE(TypeError, "put() called with undefined value.");
      }
    }
    KJ_CASE_ONEOF(o, jsg::Dict<v8::Local<v8::Value>>) {
      KJ_IF_MAYBE(v, value) {
        KJ_IF_MAYBE(opt, optionsTypeHandler.tryUnwrap(js, *v)) {
          return putMultiple(kj::mv(o), configureOptions(kj::mv(*opt)), isolate);
        } else {
          JSG_FAIL_REQUIRE(
              TypeError,
              "put() may only be called with a single key-value pair and optional options as put(key, value, options) or with multiple key-value pairs and optional options as put(entries, options)");
        }
      } else {
        return putMultiple(kj::mv(o), options, isolate);
      }
    }
  }
  KJ_UNREACHABLE;
}


jsg::Promise<void> DurableObjectStorageOperations::setAlarm(kj::Date scheduledTime,
    jsg::Optional<SetAlarmOptions> maybeOptions, v8::Isolate* isolate) {
  JSG_REQUIRE(scheduledTime > kj::origin<kj::Date>(), TypeError,
    "setAlarm() cannot be called with an alarm time <= 0");

  // This doesn't check if we have an alarm handler per say. It checks if we have an initialized
  // (post-ctor) JS durable object with an alarm handler. Notably, this means this won't throw if
  // `setAlarm` is invoked in the DO ctor even if the DO class does not have an alarm handler. This
  // is better than throwing even if we do have an alarm handler.
  IoContext::current().getActorOrThrow().assertCanSetAlarm();

  auto options = configureOptions(maybeOptions.map([](auto& o) {
    return PutOptions {
      .allowConcurrency = o.allowConcurrency,
      .allowUnconfirmed = o.allowUnconfirmed,
      .noCache = false
    };
  }).orDefault(PutOptions{}));

  // We fudge times set in the past to Date.now() to ensure that any one user can't DDOS the alarm
  // polling system by putting dates far in the past and therefore getting sorted earlier by the index.
  // This also ensures uniqueness of alarm times (which is required for correctness),
  // in the situation where customers use a constant date in the past to indicate
  // they want immediate execution.
  kj::Date dateNowKjDate =
      static_cast<int64_t>(dateNow()) * kj::MILLISECONDS + kj::UNIX_EPOCH;

  auto maybeBackpressure = transformMaybeBackpressure(isolate, options,
      getCache(OP_PUT_ALARM).setAlarm(kj::max(scheduledTime, dateNowKjDate), options));

  auto& context = IoContext::current();
  auto billProm = context.waitForOutputLocks().then([&metrics = currentActorMetrics()]() {
    // setAlarm() is billed as a single write unit.
    metrics.addStorageWriteUnits(1);
  });
  context.addTask(kj::mv(billProm));

  return kj::mv(maybeBackpressure);
}

jsg::Promise<void> DurableObjectStorageOperations::putOne(
    kj::String key, v8::Local<v8::Value> value, const PutOptions& options, v8::Isolate* isolate) {
  ActorStorageLimits::checkMaxKeySize(key);

  kj::Array<byte> buffer = serializeV8Value(value, isolate);
  ActorStorageLimits::checkMaxValueSize(key, buffer);

  auto units = billingUnits(key.size() + buffer.size());

  jsg::Promise<void> maybeBackpressure = transformMaybeBackpressure(isolate, options,
      getCache(OP_PUT).put(kj::mv(key), kj::mv(buffer), options));

  auto& context = IoContext::current();
  auto billProm = context.waitForOutputLocks().then([&metrics = currentActorMetrics(), units]() {
    metrics.addStorageWriteUnits(units);
  });
  context.addTask(kj::mv(billProm));

  return maybeBackpressure;
}

kj::OneOf<jsg::Promise<bool>, jsg::Promise<int>> DurableObjectStorageOperations::delete_(
    kj::OneOf<kj::String, kj::Array<kj::String>> keys, jsg::Optional<PutOptions> maybeOptions,
    v8::Isolate* isolate) {
  auto options = configureOptions(kj::mv(maybeOptions).orDefault(PutOptions{}));
  KJ_SWITCH_ONEOF(keys) {
    KJ_CASE_ONEOF(s, kj::String) {
      return deleteOne(kj::mv(s), options, isolate);
    }
    KJ_CASE_ONEOF(a, kj::Array<kj::String>) {
      return deleteMultiple(kj::mv(a), options, isolate);
    }
  }
  KJ_UNREACHABLE
}

jsg::Promise<void> DurableObjectStorageOperations::deleteAlarm(
    jsg::Optional<SetAlarmOptions> maybeOptions, v8::Isolate* isolate) {
  // Even if we do not have an alarm handler, we might once have had one. It's fine to remove that
  // alarm or noop on the absence of one.
  auto options = configureOptions(maybeOptions.map([](auto& o) {
    return PutOptions {
      .allowConcurrency = o.allowConcurrency,
      .allowUnconfirmed = o.allowUnconfirmed,
      .noCache = false
    };
  }).orDefault(PutOptions{}));

  return transformMaybeBackpressure(isolate, options,
      getCache(OP_DELETE_ALARM).setAlarm(nullptr, options));
}

jsg::Promise<void> DurableObjectStorage::deleteAll(
    jsg::Lock& js, jsg::Optional<PutOptions> maybeOptions) {
  auto options = configureOptions(kj::mv(maybeOptions).orDefault(PutOptions{}));

  auto deleteAll = cache->deleteAll(options);

  auto& context = IoContext::current();
  context.addTask(deleteAll.count.then([&metrics = currentActorMetrics()](uint deleted) {
    if (deleted == 0) deleted = 1;
    metrics.addStorageDeletes(deleted);
  }));

  return transformMaybeBackpressure(js.v8Isolate, options, kj::mv(deleteAll.backpressure));
}

void DurableObjectTransaction::deleteAll() {
  JSG_FAIL_REQUIRE(Error, "Cannot call deleteAll() within a transaction");
}

jsg::Promise<bool> DurableObjectStorageOperations::deleteOne(
    kj::String key, const PutOptions& options, v8::Isolate* isolate) {
  ActorStorageLimits::checkMaxKeySize(key);

  return transformCacheResult(isolate, getCache(OP_DELETE).delete_(kj::mv(key), options), options,
      [](v8::Isolate* isolate, bool value) {
    currentActorMetrics().addStorageDeletes(1);
    return value;
  });
}

jsg::Promise<jsg::Value> DurableObjectStorageOperations::getMultiple(
    kj::Array<kj::String> keys, const GetOptions& options, v8::Isolate* isolate) {
  ActorStorageLimits::checkMaxPairsCount(keys.size());

  auto numKeys = keys.size();

  return transformCacheResult(isolate, getCache(OP_GET).get(kj::mv(keys), options),
                              options, getMultipleResultsToMap(numKeys));
}

jsg::Promise<void> DurableObjectStorageOperations::putMultiple(
    jsg::Dict<v8::Local<v8::Value>> entries, const PutOptions& options, v8::Isolate* isolate) {
  kj::Vector<ActorCacheOps::KeyValuePair> kvs(entries.fields.size());

  uint32_t units = 0;
  for (auto& field : entries.fields) {
    if (field.value->IsUndefined()) continue;
    // We silently drop fields with value=undefined in putMultiple. There aren't many good options here, as
    // deleting an undefined field is confusing, throwing could break otherwise working code, and
    // a stray undefined here or there is probably closer to what the user desires.

    ActorStorageLimits::checkMaxKeySize(field.name);

    kj::Array<byte> buffer = serializeV8Value(field.value, isolate);
    ActorStorageLimits::checkMaxValueSize(field.name, buffer);

    units += billingUnits(field.name.size() + buffer.size());

    kvs.add(ActorCacheOps::KeyValuePair { kj::mv(field.name), kj::mv(buffer) });
  }

  jsg::Promise<void> maybeBackpressure = transformMaybeBackpressure(isolate, options,
      getCache(OP_PUT).put(kvs.releaseAsArray(), options));

  auto& context = IoContext::current();
  auto billProm = context.waitForOutputLocks().then([&metrics = currentActorMetrics(), units](){
    metrics.addStorageWriteUnits(units);
  });
  context.addTask(kj::mv(billProm));

  return maybeBackpressure;
}

jsg::Promise<int> DurableObjectStorageOperations::deleteMultiple(
    kj::Array<kj::String> keys, const PutOptions& options, v8::Isolate* isolate) {
  for (auto& key: keys) {
    ActorStorageLimits::checkMaxKeySize(key);
  }

  auto numKeys = keys.size();

  return transformCacheResult(isolate, getCache(OP_DELETE).delete_(kj::mv(keys), options), options,
      [numKeys](v8::Isolate*, uint count) -> int {
    currentActorMetrics().addStorageDeletes(numKeys);
    return count;
  });
}

ActorCacheOps& DurableObjectStorage::getCache(OpName op) {
  return *cache;
}

jsg::Promise<jsg::Value> DurableObjectStorage::transaction(jsg::Lock& js,
    jsg::Function<jsg::Promise<jsg::Value>(jsg::Ref<DurableObjectTransaction>)> callback,
    jsg::Optional<TransactionOptions> options) {
  auto& context = IoContext::current();

  struct TxnResult {
    jsg::Value value;
    bool isError;
  };

  return context.blockConcurrencyWhile(js,
      [callback = kj::mv(callback), &context, &cache = *cache]
      (jsg::Lock& js) mutable -> jsg::Promise<TxnResult> {
    // Note that the call to `startTransaction()` is when the SQLite-backed implementation will
    // actually invoke `BEGIN TRANSACTION`, so it's important that we're inside the
    // blockConcurrencyWhile block before that point so we don't accidentally catch some other
    // asynchronous event in our transaction.
    //
    // For the ActorCache-based implementation, it doesn't matter when we call `startTransaction()`
    // as the method merely allocates an object and returns it with no side effects.
    auto txn = jsg::alloc<DurableObjectTransaction>(context.addObject(cache.startTransaction()));

    return js.resolvedPromise(txn.addRef())
        .then(js, kj::mv(callback))
        .then(js, [txn = txn.addRef()](jsg::Lock& js, jsg::Value value) mutable {
      // In correct usage, `context` should not have changed here, particularly because we're in
      // a critical section so it should have been impossible for any other context to receive
      // control. However, depending on all that is a bit precarious. jsg::Promise::then() itself
      // does NOT guarantee it runs in the same context (the application could have returned a
      // custom Promise and then resolved in from some other context). So let's be safe and grab
      // IoContext::current() again here, rather than capture it in the lambda.
      auto& context = IoContext::current();
      return context.awaitIoWithInputLock(txn->maybeCommit(), [value = kj::mv(value)]() mutable {
        return TxnResult { kj::mv(value), false };
      });
    }, [txn = txn.addRef()](jsg::Lock& js, jsg::Value exception) mutable {
      // The transaction callback threw an exception. We don't actually want to reset the object,
      // we only want to roll back the transaction and propagate the exception. So, we carefully
      // pack the exception away into a value.
      txn->maybeRollback();
      return js.resolvedPromise(TxnResult { kj::mv(exception), true });
    });
  }).then(js, [](jsg::Lock& js, TxnResult result) -> jsg::Value {
    if (result.isError) {
      js.throwException(kj::mv(result.value));
    } else {
      return kj::mv(result.value);
    }
  });
}

jsg::Value DurableObjectStorage::transactionSync(jsg::Lock& js, jsg::Function<jsg::Value()> callback) {
  KJ_IF_MAYBE(sqlite, cache->getSqliteDatabase()) {
    // SAVEPOINT is a readonly statement, but we need to trigger an outer TRANSACTION
    sqlite->notifyWrite();
    // TODO(sqlite) allow nested savepoints, using a new savepoint name for each depth
    sqlite->run("SAVEPOINT _cf_savepoint;");
    return js.tryCatch([&]() {
      auto result = callback(js);
      sqlite->run("RELEASE _cf_savepoint;");
      return kj::mv(result);
    }, [&](jsg::Value exception) -> jsg::Value {
      sqlite->run("ROLLBACK TO _cf_savepoint;");
      js.throwException(kj::mv(exception));
    });
  } else {
    JSG_FAIL_REQUIRE(Error, "Durable Object is not backed by SQL.");
  }
}

jsg::Promise<void> DurableObjectStorage::sync(jsg::Lock& js) {
  KJ_IF_MAYBE(p, cache->onNoPendingFlush()) {
    // Note that we're not actually flushing since that will happen anyway once we go async. We're
    // merely checking if we have any pending or in-flight operations, and providing a promise that
    // resolves when they succeed. This promise only covers operations that were scheduled before
    // this method was invoked. If the cache has to flush again later from future operations, this
    // promise will resolve before they complete. If this promise were to reject, then the actor's
    // output gate will be broken first and the isolate will not resume synchronous execution.

    auto& context = IoContext::current();
    return context.awaitIo(kj::mv(*p));
  } else {
    return js.resolvedPromise();
  }
}

jsg::Ref<SqlStorage> DurableObjectStorage::getSql(jsg::Lock& js) {
  KJ_IF_MAYBE(db, cache->getSqliteDatabase()) {
    return jsg::alloc<SqlStorage>(*db, JSG_THIS);
  } else {
    JSG_FAIL_REQUIRE(Error, "Durable Object is not backed by SQL.");
  }
}

ActorCacheOps& DurableObjectTransaction::getCache(OpName op) {
  JSG_REQUIRE(!rolledBack, Error, kj::str("Cannot ", op, " on rolled back transaction"));
  auto& result = *JSG_REQUIRE_NONNULL(cacheTxn, Error,
      kj::str("Cannot call ", op,
      " on transaction that has already committed: did you move `txn` outside of the closure?"));
  return result;
}

void DurableObjectTransaction::rollback() {
  if (rolledBack) return;  // allow multiple calls to rollback()
  getCache(OP_ROLLBACK);  // just for the checks
  KJ_IF_MAYBE(t, cacheTxn) {
    auto prom = (*t)->rollback();
    IoContext::current().addWaitUntil(kj::mv(prom).attach(kj::mv(cacheTxn)));
    cacheTxn = nullptr;
  }
  rolledBack = true;
}

kj::Promise<void> DurableObjectTransaction::maybeCommit() {
  // cacheTxn is null if rollback() was called, in which case we don't want to commit anything.
  KJ_IF_MAYBE(t, cacheTxn) {
    auto maybePromise = (*t)->commit();
    cacheTxn = nullptr;
    KJ_IF_MAYBE(promise, maybePromise) {
      return kj::mv(*promise);
    }
  }
  return kj::READY_NOW;
}

void DurableObjectTransaction::maybeRollback() {
  cacheTxn = nullptr;
  rolledBack = true;
}

ActorState::ActorState(Worker::Actor::Id actorId,
    kj::Maybe<jsg::Value> transient, kj::Maybe<jsg::Ref<DurableObjectStorage>> persistent)
    : id(kj::mv(actorId)), transient(kj::mv(transient)), persistent(kj::mv(persistent)) {}

kj::OneOf<jsg::Ref<DurableObjectId>, kj::StringPtr> ActorState::getId() {
  KJ_SWITCH_ONEOF(id) {
    KJ_CASE_ONEOF(coloLocalId, kj::String) {
      return coloLocalId.asPtr();
    }
    KJ_CASE_ONEOF(globalId, kj::Own<ActorIdFactory::ActorId>) {
      return jsg::alloc<DurableObjectId>(globalId->clone());
    }
  }
  KJ_UNREACHABLE;
}

DurableObjectState::DurableObjectState(Worker::Actor::Id actorId,
    kj::Maybe<jsg::Ref<DurableObjectStorage>> storage)
    : id(kj::mv(actorId)), storage(kj::mv(storage)) {}

void DurableObjectState::waitUntil(kj::Promise<void> promise) {
  IoContext::current().addWaitUntil(kj::mv(promise));
}

kj::OneOf<jsg::Ref<DurableObjectId>, kj::StringPtr> DurableObjectState::getId() {
  KJ_SWITCH_ONEOF(id) {
    KJ_CASE_ONEOF(coloLocalId, kj::String) {
      return coloLocalId.asPtr();
    }
    KJ_CASE_ONEOF(globalId, kj::Own<ActorIdFactory::ActorId>) {
      return jsg::alloc<DurableObjectId>(globalId->clone());
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<jsg::Value> DurableObjectState::blockConcurrencyWhile(jsg::Lock& js,
    jsg::Function<jsg::Promise<jsg::Value>()> callback) {
  return IoContext::current().blockConcurrencyWhile(js, kj::mv(callback));
}

void DurableObjectState::abort(jsg::Optional<kj::String> reason) {
  kj::String description = kj::mv(reason).map([](kj::String&& text) {
    return kj::str("broken.outputGateBroken; jsg.Error: ", text);
  }).orDefault([]() {
    return kj::str("broken.outputGateBroken; jsg.Error: Application called abort() to reset "
        "Durable Object.");
  });

  kj::Exception error(kj::Exception::Type::FAILED, __FILE__, __LINE__, kj::mv(description));

  KJ_IF_MAYBE(s, storage) {
    // Make sure we _synchronously_ break storage so that there's no chance our promise fulfilling
    // will race against the output gate, possibly allowing writes to complete before being
    // canceled.
    s->get()->getActorCacheInterface().shutdown(error);
  }

  IoContext::current().abort(kj::cp(error));
  kj::throwFatalException(kj::mv(error));
}

void DurableObjectState::acceptWebSocket(
    jsg::Ref<WebSocket> ws,
    jsg::Optional<kj::Array<kj::String>> tags) {
  JSG_ASSERT(!ws->isAccepted(), Error,
      "Cannot call `acceptWebSocket()` if the WebSocket was already accepted via `accept()`");
  JSG_ASSERT(ws->pairIsAwaitingCoupling(), Error,
      "Cannot call `acceptWebSocket()` on this WebSocket because its pair has already been "\
      "accepted or used in a Response.");
  // WebSocket::couple() will keep the IoContext around if the websocket we return in the Response
  // is `LOCAL`, so we have to set it to remote. Note that `setRemoteOnPair()` will throw if
  // `ws` is not an end of a WebSocketPair.
  ws->setRemoteOnPair();

  // We need to get a HibernationManager to give the websocket to.
  auto& a = KJ_REQUIRE_NONNULL(IoContext::current().getActor());
  if (a.getHibernationManager() == nullptr) {
    a.setHibernationManager(
        kj::refcounted<HibernationManagerImpl>(
            a.getLoopback(), KJ_REQUIRE_NONNULL(a.getHibernationEventType())));
  }
  // HibernationManager's acceptWebSocket() will throw if the websocket is in an incompatible state.
  // Note that not providing a tag is equivalent to providing an empty tag array.
  // Any duplicate tags will be ignored.
  kj::Array<kj::String> distinctTags = [&]() -> kj::Array<kj::String> {
    KJ_IF_MAYBE(t, tags) {
      kj::HashSet<kj::String> seen;
      size_t distinctTagCount = 0;
      for (auto tag = t->begin(); tag < t->end(); tag++) {
        JSG_REQUIRE(distinctTagCount < MAX_TAGS_PER_CONNECTION, Error,
            "a Hibernatable WebSocket cannot have more than ", MAX_TAGS_PER_CONNECTION, " tags");
        JSG_REQUIRE(tag->size() <= MAX_TAG_LENGTH, Error,
            "\"", *tag, "\" ",
            "is longer than the max tag length (", MAX_TAG_LENGTH, " characters).");
        if (!seen.contains(*tag)) {
          seen.insert(kj::mv(*tag));
          distinctTagCount++;
        }
      }

      return KJ_MAP(tag, seen) { return kj::mv(tag); };
    }
    return kj::Array<kj::String>();
  }();
  KJ_REQUIRE_NONNULL(a.getHibernationManager()).acceptWebSocket(kj::mv(ws), distinctTags);
}

kj::Array<jsg::Ref<api::WebSocket>> DurableObjectState::getWebSockets(
    jsg::Lock& js,
    jsg::Optional<kj::String> tag) {
  auto& a = KJ_REQUIRE_NONNULL(IoContext::current().getActor());
  KJ_IF_MAYBE(manager, a.getHibernationManager()) {
    return manager->getWebSockets(
        js, tag.map([](kj::StringPtr t) { return t; })).releaseAsArray();
  }
  return kj::Array<jsg::Ref<api::WebSocket>>();
}

kj::Array<kj::byte> serializeV8Value(v8::Local<v8::Value> value, v8::Isolate* isolate) {
  jsg::Serializer serializer(isolate, jsg::Serializer::Options {
    .version = 15,
    .omitHeader = false,
  });
  serializer.write(value);
  auto released = serializer.release();
  return kj::mv(released.data);
}

v8::Local<v8::Value> deserializeV8Value(
    kj::ArrayPtr<const char> key, kj::ArrayPtr<const kj::byte> buf, v8::Isolate* isolate) {

  v8::TryCatch tryCatch(isolate);

  // We explicitly check and assert on the results of v8 method calls instead of using jsg::check so
  // that it will be logged, since exceptions thrown by v8 are typically not logged but we obviously
  // really want to know about any potential data corruption issues. We use v8::TryCatch so that we
  // can access the error message from v8 and include it in our log message.
  //
  // If we do hit a deserialization error, we log information that will be helpful in understanding
  // the problem but that won't leak too much about the customer's data. We include the key (to help
  // find the data in the database if it hasn't been deleted), the length of the value, and the
  // first three bytes of the value (which is just the v8-internal version header and the tag that
  // indicates the type of the value, but not its contents).
  auto handleV8Exception = [&](kj::StringPtr errorMsg) {
    // We can occasionally hit an isolate termination here -- we prefix the error with jsg to avoid
    // counting it against our internal storage error metrics but also throw a KJ exception rather
    // than a jsExceptionThrown error to avoid confusing the normal termination handling code.
    // We don't expect users to ever actually see this error.
    JSG_REQUIRE(!tryCatch.HasTerminated(),
        Error, "isolate terminated while deserializing value from Durable Object storage; "
        "contact us if you're wondering why you're seeing this");
    if (tryCatch.Message().IsEmpty()) {
      // This also should never happen, but check for it because otherwise V8 will crash.
      KJ_LOG(ERROR, "tryCatch.Message() was empty even when not HasTerminated()??");
      KJ_FAIL_ASSERT("unexpectedly missing JS exception in actor storage deserialization failure",
          errorMsg, key, buf.size(), buf.slice(0, std::min(static_cast<size_t>(3), buf.size())));
    }
    auto jsException = tryCatch.Exception();
    KJ_FAIL_ASSERT("actor storage deserialization failed", errorMsg, jsException,
        key, buf.size(), buf.slice(0, std::min(static_cast<size_t>(3), buf.size())));
  };

  KJ_ASSERT(buf.size() > 0, "unexpectedly empty value buffer", key);

  jsg::Deserializer::Options options {};
  if (buf[0] != 0xFF) {
    // When Durable Objects was first released, it did not properly write headers when serializing
    // to storage. If we find that the header is missing (as indicated by the first byte not being
    // 0xFF), it's safe to assume that the data was written at the only serialization version we
    // used during that early time period, so we explicitly set that version here.
    options.version = 13;
    options.readHeader = false;
  }

  jsg::Deserializer deserializer(isolate, buf, nullptr, nullptr, options);

  v8::Local<v8::Value> value;
  try {
    value = deserializer.readValue();
  } catch (jsg::JsExceptionThrown&) {
    KJ_ASSERT(tryCatch.HasCaught());
    handleV8Exception("failed to deserialize stored value");
  }
  return value;
}

}  // namespace workerd::api
