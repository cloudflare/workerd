// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-state.h"

#include "actor.h"
#include "sql.h"
#include "util.h"

#include <workerd/api/web-socket.h>
#include <workerd/io/actor-cache.h>
#include <workerd/io/actor-id.h>
#include <workerd/io/actor-sqlite.h>
#include <workerd/io/features.h>
#include <workerd/io/hibernation-manager.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>
#include <workerd/jsg/util.h>

#include <v8.h>

namespace workerd::api {

namespace {

constexpr size_t BILLING_UNIT = 4096;

enum class BillAtLeastOne { NO, YES };

uint32_t billingUnits(size_t bytes, BillAtLeastOne billAtLeastOne = BillAtLeastOne::YES) {
  if (billAtLeastOne == BillAtLeastOne::YES && bytes == 0) {
    return 1;  // always bill for at least 1 billing unit
  }
  return bytes / BILLING_UNIT + (bytes % BILLING_UNIT != 0);
}

jsg::JsValue deserializeMaybeV8Value(
    jsg::Lock& js, kj::ArrayPtr<const char> key, kj::Maybe<kj::ArrayPtr<const kj::byte>> buf) {
  KJ_IF_SOME(b, buf) {
    return deserializeV8Value(js, key, b);
  } else {
    return js.undefined();
  }
}

template <typename T, typename Options, typename Func>
auto transformCacheResult(jsg::Lock& js,
    kj::OneOf<T, kj::Promise<T>> input,
    const Options& options,
    Func&& func) -> jsg::Promise<decltype(func(js, kj::instance<T>()))> {
  KJ_SWITCH_ONEOF(input) {
    KJ_CASE_ONEOF(value, T) {
      return js.resolvedPromise(func(js, kj::mv(value)));
    }
    KJ_CASE_ONEOF(promise, kj::Promise<T>) {
      auto& context = IoContext::current();
      if (options.allowConcurrency.orDefault(false)) {
        return context.awaitIo(
            js, kj::mv(promise), [func = kj::fwd<Func>(func)](jsg::Lock& js, T&& value) mutable {
          return func(js, kj::mv(value));
        });
      } else {
        return context.awaitIoWithInputLock(
            js, kj::mv(promise), [func = kj::fwd<Func>(func)](jsg::Lock& js, T&& value) mutable {
          return func(js, kj::mv(value));
        });
      }
    }
  }
  KJ_UNREACHABLE;
}

template <typename T, typename Options, typename Func>
auto transformCacheResultWithCacheStatus(jsg::Lock& js,
    kj::OneOf<T, kj::Promise<T>> input,
    const Options& options,
    Func&& func) -> jsg::Promise<decltype(func(js, kj::instance<T>(), kj::instance<bool>()))> {
  KJ_SWITCH_ONEOF(input) {
    KJ_CASE_ONEOF(value, T) {
      return js.resolvedPromise(func(js, kj::mv(value), true));
    }
    KJ_CASE_ONEOF(promise, kj::Promise<T>) {
      auto& context = IoContext::current();
      if (options.allowConcurrency.orDefault(false)) {
        return context.awaitIo(
            js, kj::mv(promise), [func = kj::fwd<Func>(func)](jsg::Lock& js, T&& value) mutable {
          return func(js, kj::mv(value), false);
        });
      } else {
        return context.awaitIoWithInputLock(
            js, kj::mv(promise), [func = kj::fwd<Func>(func)](jsg::Lock& js, T&& value) mutable {
          return func(js, kj::mv(value), false);
        });
      }
    }
  }
  KJ_UNREACHABLE;
}

template <typename Options>
jsg::Promise<void> transformMaybeBackpressure(
    jsg::Lock& js, const Options& options, kj::Maybe<kj::Promise<void>> maybeBackpressure) {
  KJ_IF_SOME(backpressure, maybeBackpressure) {
    // Note: In practice `allowConcurrency` will have no effect on a backpressure promise since
    //   backpressure blocks everything anyway, but we pass the option through for consistency in
    //   case of future changes.
    auto& context = IoContext::current();
    if (options.allowConcurrency.orDefault(false)) {
      return context.awaitIo(js, kj::mv(backpressure));
    } else {
      return context.awaitIoWithInputLock(js, kj::mv(backpressure), [](jsg::Lock&) {});
    }
  } else {
    return js.resolvedPromise();
  }
}

ActorObserver& currentActorMetrics() {
  return IoContext::current().getActorOrThrow().getMetrics();
}

jsg::JsRef<jsg::JsValue> listResultsToMap(
    jsg::Lock& js, ActorCacheOps::GetResultList value, bool completelyCached) {
  return js.withinHandleScope([&] {
    auto map = js.map();
    size_t cachedReadBytes = 0;
    size_t uncachedReadBytes = 0;
    for (auto entry: value) {
      auto& bytesRef =
          entry.status == ActorCacheOps::CacheStatus::CACHED ? cachedReadBytes : uncachedReadBytes;
      bytesRef += entry.key.size() + entry.value.size();
      map.set(js, entry.key, deserializeV8Value(js, entry.key, entry.value));
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

    return jsg::JsValue(map).addRef(js);
  });
}

kj::Function<jsg::JsRef<jsg::JsValue>(jsg::Lock&, ActorCacheOps::GetResultList)>
getMultipleResultsToMap(size_t numInputKeys) {
  return [numInputKeys](jsg::Lock& js, ActorCacheOps::GetResultList value) mutable {
    return js.withinHandleScope([&] {
      auto map = js.map();
      uint32_t cachedUnits = 0;
      uint32_t uncachedUnits = 0;
      for (auto entry: value) {
        auto& unitsRef =
            entry.status == ActorCacheOps::CacheStatus::CACHED ? cachedUnits : uncachedUnits;
        unitsRef += billingUnits(entry.key.size() + entry.value.size());
        map.set(js, entry.key, deserializeV8Value(js, entry.key, entry.value));
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

      return jsg::JsValue(map).addRef(js);
    });
  };
}

kj::Promise<void> updateStorageWriteUnit(
    IoContext& context, ActorObserver& metrics, uint32_t units) {
  // The ActorObserver& reference here is guaranteed to outlive this task, so
  // accessing it after the co_await here is safe.
  co_await context.waitForOutputLocks();
  metrics.addStorageWriteUnits(units);
}

kj::Promise<void> updateStorageDeletes(
    IoContext& context, ActorObserver& metrics, kj::Promise<uint> promise) {
  // The ActorObserver& reference here is guaranteed to outlive this task, so
  // accessing it after the co_await here is safe.
  auto deleted = co_await promise;
  if (deleted == 0) deleted = 1;
  metrics.addStorageDeletes(deleted);
};

// Return the id of the current actor (or the empty string if there is no current actor).
kj::Maybe<kj::String> getCurrentActorId() {
  if (IoContext::hasCurrent()) {
    IoContext& ioContext = IoContext::current();
    KJ_IF_SOME(actor, ioContext.getActor()) {
      KJ_SWITCH_ONEOF(actor.getId()) {
        KJ_CASE_ONEOF(s, kj::String) {
          return kj::heapString(s);
        }
        KJ_CASE_ONEOF(actorId, kj::Own<ActorIdFactory::ActorId>) {
          return actorId->toString();
        }
      }
      KJ_UNREACHABLE;
    }
  }
  return kj::none;
}

}  // namespace

DurableObjectStorage::DurableObjectStorage(IoPtr<ActorCacheInterface> cache,
    bool enableSql,
    kj::Own<IoChannelFactory::ActorChannel> primaryActorChannel,
    kj::Own<ActorIdFactory::ActorId> primaryActorId)
    : cache(kj::mv(cache)),
      enableSql(enableSql) {

  auto replicaFactory = kj::heap<ReplicaActorOutgoingFactory>(
      kj::mv(primaryActorChannel), primaryActorId->toString());
  auto outgoingFactory =
      IoContext::current().addObject<Fetcher::OutgoingFactory>(kj::mv(replicaFactory));
  auto requiresHost = FeatureFlags::get(IoContext::current().getCurrentLock())
                          .getDurableObjectFetchRequiresSchemeAuthority()
      ? Fetcher::RequiresHostAndProtocol::YES
      : Fetcher::RequiresHostAndProtocol::NO;

  this->maybePrimary = jsg::alloc<DurableObject>(
      jsg::alloc<DurableObjectId>(kj::mv(primaryActorId)), kj::mv(outgoingFactory), requiresHost);
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> DurableObjectStorageOperations::get(jsg::Lock& js,
    kj::OneOf<kj::String, kj::Array<kj::String>> keys,
    jsg::Optional<GetOptions> maybeOptions) {
  auto options = configureOptions(kj::mv(maybeOptions).orDefault(GetOptions{}));
  KJ_SWITCH_ONEOF(keys) {
    KJ_CASE_ONEOF(s, kj::String) {
      return getOne(js, kj::mv(s), options);
    }
    KJ_CASE_ONEOF(a, kj::Array<kj::String>) {
      return getMultiple(js, kj::mv(a), options);
    }
  }
  KJ_UNREACHABLE
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> DurableObjectStorageOperations::getOne(
    jsg::Lock& js, kj::String key, const GetOptions& options) {
  auto result = getCache(OP_GET).get(kj::str(key), options);
  return transformCacheResultWithCacheStatus(js, kj::mv(result), options,
      [key = kj::mv(key)](jsg::Lock& js, kj::Maybe<ActorCacheOps::Value> value, bool cached) {
    uint32_t units = 1;
    KJ_IF_SOME(v, value) {
      units = billingUnits(v.size());
    }
    auto& actorMetrics = currentActorMetrics();
    if (cached) {
      actorMetrics.addCachedStorageReadUnits(units);
    } else {
      actorMetrics.addUncachedStorageReadUnits(units);
    }
    return deserializeMaybeV8Value(js, key, value).addRef(js);
  });
}

jsg::Promise<kj::Maybe<double>> DurableObjectStorageOperations::getAlarm(
    jsg::Lock& js, jsg::Optional<GetAlarmOptions> maybeOptions) {
  // Even if we do not have an alarm handler, we might once have had one. It's fine to return
  // whatever a previous alarm setting or a falsy result.
  auto options = configureOptions(maybeOptions
                                      .map([](auto& o) {
    return GetOptions{.allowConcurrency = o.allowConcurrency, .noCache = false};
  }).orDefault(GetOptions{}));
  auto result = getCache(OP_GET_ALARM).getAlarm(options);

  return transformCacheResult(
      js, kj::mv(result), options, [](jsg::Lock&, kj::Maybe<kj::Date> date) {
    return date.map(
        [](auto& date) { return static_cast<double>((date - kj::UNIX_EPOCH) / kj::MILLISECONDS); });
  });
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> DurableObjectStorageOperations::list(
    jsg::Lock& js, jsg::Optional<ListOptions> maybeOptions) {
  kj::String start;
  kj::Maybe<kj::String> end;
  bool reverse = false;
  kj::Maybe<uint> limit;

  auto makeEmptyResult = [&]() { return js.resolvedPromise(jsg::JsValue(js.map()).addRef(js)); };

  KJ_IF_SOME(o, maybeOptions) {
    KJ_IF_SOME(s, o.start) {
      if (o.startAfter != kj::none) {
        KJ_FAIL_REQUIRE(
            "jsg.TypeError: list() cannot be called with both start and startAfter values.");
      }
      start = kj::mv(s);
    }
    KJ_IF_SOME(sks, o.startAfter) {
      // Convert an exclusive startAfter into an inclusive start key here so that the implementation
      // doesn't need to handle both. This can be done simply by adding two NULL bytes. One to the end of
      // the startAfter and another to set the start key after startAfter.
      auto startAfterKey = kj::heapArray<char>(sks.size() + 2);

      // Copy over the original string.
      memcpy(startAfterKey.begin(), sks.begin(), sks.size());
      // Add one additional null byte to set the new start as the key immediately
      // after startAfter. This looks a little sketchy to be doing with strings rather
      // than arrays, but kj::String explicitly allows for NULL bytes inside of strings.
      startAfterKey[startAfterKey.size() - 2] = '\0';
      // kj::String automatically reads the last NULL as string termination, so we need to add it twice
      // to make it stick in the final string.
      startAfterKey[startAfterKey.size() - 1] = '\0';
      start = kj::String(kj::mv(startAfterKey));
    }
    KJ_IF_SOME(e, o.end) {
      end = kj::mv(e);
    }
    KJ_IF_SOME(r, o.reverse) {
      reverse = r;
    }
    KJ_IF_SOME(l, o.limit) {
      JSG_REQUIRE(l > 0, TypeError, "List limit must be positive.");
      limit = l;
    }
    KJ_IF_SOME(prefix, o.prefix) {
      // Let's clamp `start` and `end` to include only keys with the given prefix.
      if (prefix.size() > 0) {
        if (start < prefix) {
          // `start` is before `prefix`, so listing should actually start at `prefix`.
          start = kj::str(prefix);
        } else if (start.startsWith(prefix)) {
          // `start` is within the prefix, so need not be modified.
        } else {
          // `start` comes after the last value with the prefix, so there's no overlap.
          return makeEmptyResult();
        }

        // Calculate the first key that sorts after all keys with the given prefix.
        kj::Vector<char> keyAfterPrefix(prefix.size());
        keyAfterPrefix.addAll(prefix);
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

          KJ_IF_SOME(e, end) {
            if (e <= prefix) {
              // No keys could possibly match both the end and the prefix.
              return makeEmptyResult();
            } else if (e.startsWith(prefix)) {
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

  KJ_IF_SOME(e, end) {
    if (e <= start) {
      // Key range is empty.
      return makeEmptyResult();
    }
  }

  auto options = configureOptions(kj::mv(maybeOptions).orDefault(ListOptions{}));
  ActorCacheOps::ReadOptions readOptions = options;

  auto result = reverse
      ? getCache(OP_LIST).listReverse(kj::mv(start), kj::mv(end), limit, readOptions)
      : getCache(OP_LIST).list(kj::mv(start), kj::mv(end), limit, readOptions);
  return transformCacheResultWithCacheStatus(js, kj::mv(result), options, &listResultsToMap);
}

jsg::Promise<void> DurableObjectStorageOperations::put(jsg::Lock& js,
    kj::OneOf<kj::String, jsg::Dict<jsg::JsValue>> keyOrEntries,
    jsg::Optional<jsg::JsValue> value,
    jsg::Optional<PutOptions> maybeOptions,
    const jsg::TypeHandler<PutOptions>& optionsTypeHandler) {
  // TODO(soon): Add tests of data generated at current versions to ensure we'll
  // know before releasing any backwards-incompatible serializer changes,
  // potentially checking the header in addition to the value.
  auto options = configureOptions(kj::mv(maybeOptions).orDefault(PutOptions{}));
  KJ_SWITCH_ONEOF(keyOrEntries) {
    KJ_CASE_ONEOF(k, kj::String) {
      KJ_IF_SOME(v, value) {
        return putOne(js, kj::mv(k), v, options);
      } else {
        JSG_FAIL_REQUIRE(TypeError, "put() called with undefined value.");
      }
    }
    KJ_CASE_ONEOF(o, jsg::Dict<jsg::JsValue>) {
      KJ_IF_SOME(v, value) {
        KJ_IF_SOME(opt, optionsTypeHandler.tryUnwrap(js, v)) {
          return putMultiple(js, kj::mv(o), configureOptions(kj::mv(opt)));
        } else {
          JSG_FAIL_REQUIRE(TypeError,
              "put() may only be called with a single key-value pair and optional options as put(key, value, options) or with multiple key-value pairs and optional options as put(entries, options)");
        }
      } else {
        return putMultiple(js, kj::mv(o), options);
      }
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<void> DurableObjectStorageOperations::setAlarm(
    jsg::Lock& js, kj::Date scheduledTime, jsg::Optional<SetAlarmOptions> maybeOptions) {
  JSG_REQUIRE(scheduledTime > kj::origin<kj::Date>(), TypeError,
      "setAlarm() cannot be called with an alarm time <= 0");

  auto& context = IoContext::current();
  // This doesn't check if we have an alarm handler per say. It checks if we have an initialized
  // (post-ctor) JS durable object with an alarm handler. Notably, this means this won't throw if
  // `setAlarm` is invoked in the DO ctor even if the DO class does not have an alarm handler. This
  // is better than throwing even if we do have an alarm handler.
  context.getActorOrThrow().assertCanSetAlarm();

  auto options = configureOptions(maybeOptions
                                      .map([](auto& o) {
    return PutOptions{.allowConcurrency = o.allowConcurrency,
      .allowUnconfirmed = o.allowUnconfirmed,
      .noCache = false};
  }).orDefault(PutOptions{}));

  // We fudge times set in the past to Date.now() to ensure that any one user can't DDOS the alarm
  // polling system by putting dates far in the past and therefore getting sorted earlier by the index.
  // This also ensures uniqueness of alarm times (which is required for correctness),
  // in the situation where customers use a constant date in the past to indicate
  // they want immediate execution.
  kj::Date dateNowKjDate = static_cast<int64_t>(dateNow()) * kj::MILLISECONDS + kj::UNIX_EPOCH;

  auto maybeBackpressure = transformMaybeBackpressure(
      js, options, getCache(OP_PUT_ALARM).setAlarm(kj::max(scheduledTime, dateNowKjDate), options));

  // setAlarm() is billed as a single write unit.
  context.addTask(updateStorageWriteUnit(context, currentActorMetrics(), 1));

  return kj::mv(maybeBackpressure);
}

jsg::Promise<void> DurableObjectStorageOperations::putOne(
    jsg::Lock& js, kj::String key, jsg::JsValue value, const PutOptions& options) {
  kj::Array<byte> buffer = serializeV8Value(js, value);

  auto units = billingUnits(key.size() + buffer.size());

  jsg::Promise<void> maybeBackpressure = transformMaybeBackpressure(
      js, options, getCache(OP_PUT).put(kj::mv(key), kj::mv(buffer), options));

  auto& context = IoContext::current();
  context.addTask(updateStorageWriteUnit(context, currentActorMetrics(), units));

  return maybeBackpressure;
}

kj::OneOf<jsg::Promise<bool>, jsg::Promise<int>> DurableObjectStorageOperations::delete_(
    jsg::Lock& js,
    kj::OneOf<kj::String, kj::Array<kj::String>> keys,
    jsg::Optional<PutOptions> maybeOptions) {
  auto options = configureOptions(kj::mv(maybeOptions).orDefault(PutOptions{}));
  KJ_SWITCH_ONEOF(keys) {
    KJ_CASE_ONEOF(s, kj::String) {
      return deleteOne(js, kj::mv(s), options);
    }
    KJ_CASE_ONEOF(a, kj::Array<kj::String>) {
      return deleteMultiple(js, kj::mv(a), options);
    }
  }
  KJ_UNREACHABLE
}

jsg::Promise<void> DurableObjectStorageOperations::deleteAlarm(
    jsg::Lock& js, jsg::Optional<SetAlarmOptions> maybeOptions) {
  // Even if we do not have an alarm handler, we might once have had one. It's fine to remove that
  // alarm or noop on the absence of one.
  auto options = configureOptions(maybeOptions
                                      .map([](auto& o) {
    return PutOptions{.allowConcurrency = o.allowConcurrency,
      .allowUnconfirmed = o.allowUnconfirmed,
      .noCache = false};
  }).orDefault(PutOptions{}));

  return transformMaybeBackpressure(
      js, options, getCache(OP_DELETE_ALARM).setAlarm(kj::none, options));
}

jsg::Promise<void> DurableObjectStorage::deleteAll(
    jsg::Lock& js, jsg::Optional<PutOptions> maybeOptions) {
  auto options = configureOptions(kj::mv(maybeOptions).orDefault(PutOptions{}));

  auto deleteAll = cache->deleteAll(options);

  auto& context = IoContext::current();
  context.addTask(updateStorageDeletes(context, currentActorMetrics(), kj::mv(deleteAll.count)));

  return transformMaybeBackpressure(js, options, kj::mv(deleteAll.backpressure));
}

void DurableObjectTransaction::deleteAll() {
  JSG_FAIL_REQUIRE(Error, "Cannot call deleteAll() within a transaction");
}

jsg::Promise<bool> DurableObjectStorageOperations::deleteOne(
    jsg::Lock& js, kj::String key, const PutOptions& options) {
  return transformCacheResult(
      js, getCache(OP_DELETE).delete_(kj::mv(key), options), options, [](jsg::Lock&, bool value) {
    currentActorMetrics().addStorageDeletes(1);
    return value;
  });
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> DurableObjectStorageOperations::getMultiple(
    jsg::Lock& js, kj::Array<kj::String> keys, const GetOptions& options) {
  auto numKeys = keys.size();

  return transformCacheResult(
      js, getCache(OP_GET).get(kj::mv(keys), options), options, getMultipleResultsToMap(numKeys));
}

jsg::Promise<void> DurableObjectStorageOperations::putMultiple(
    jsg::Lock& js, jsg::Dict<jsg::JsValue> entries, const PutOptions& options) {
  kj::Vector<ActorCacheOps::KeyValuePair> kvs(entries.fields.size());

  uint32_t units = 0;
  for (auto& field: entries.fields) {
    if (field.value.isUndefined()) continue;
    // We silently drop fields with value=undefined in putMultiple. There aren't many good options here, as
    // deleting an undefined field is confusing, throwing could break otherwise working code, and
    // a stray undefined here or there is probably closer to what the user desires.

    kj::Array<byte> buffer = serializeV8Value(js, field.value);

    units += billingUnits(field.name.size() + buffer.size());

    kvs.add(ActorCacheOps::KeyValuePair{kj::mv(field.name), kj::mv(buffer)});
  }

  jsg::Promise<void> maybeBackpressure =
      transformMaybeBackpressure(js, options, getCache(OP_PUT).put(kvs.releaseAsArray(), options));

  auto& context = IoContext::current();
  context.addTask(updateStorageWriteUnit(context, currentActorMetrics(), units));

  return maybeBackpressure;
}

jsg::Promise<int> DurableObjectStorageOperations::deleteMultiple(
    jsg::Lock& js, kj::Array<kj::String> keys, const PutOptions& options) {
  auto numKeys = keys.size();

  return transformCacheResult(js, getCache(OP_DELETE).delete_(kj::mv(keys), options), options,
      [numKeys](jsg::Lock&, uint count) -> int {
    currentActorMetrics().addStorageDeletes(numKeys);
    return count;
  });
}

ActorCacheOps& DurableObjectStorage::getCache(OpName op) {
  return *cache;
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> DurableObjectStorage::transaction(jsg::Lock& js,
    jsg::Function<jsg::Promise<jsg::JsRef<jsg::JsValue>>(jsg::Ref<DurableObjectTransaction>)>
        callback,
    jsg::Optional<TransactionOptions> options) {
  auto& context = IoContext::current();

  struct TxnResult {
    jsg::JsRef<jsg::JsValue> value;
    bool isError;
  };

  return context
      .blockConcurrencyWhile(js,
          [callback = kj::mv(callback), &context, &cache = *cache](
              jsg::Lock& js) mutable -> jsg::Promise<TxnResult> {
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
        .then(js, [txn = txn.addRef()](jsg::Lock& js, jsg::JsRef<jsg::JsValue> value) mutable {
      // In correct usage, `context` should not have changed here, particularly because we're in
      // a critical section so it should have been impossible for any other context to receive
      // control. However, depending on all that is a bit precarious. jsg::Promise::then() itself
      // does NOT guarantee it runs in the same context (the application could have returned a
      // custom Promise and then resolved in from some other context). So let's be safe and grab
      // IoContext::current() again here, rather than capture it in the lambda.
      auto& context = IoContext::current();
      return context.awaitIoWithInputLock(js, txn->maybeCommit(),
          [value = kj::mv(value)](jsg::Lock&) mutable { return TxnResult{kj::mv(value), false}; });
    }, [txn = txn.addRef()](jsg::Lock& js, jsg::Value exception) mutable {
      // The transaction callback threw an exception. We don't actually want to reset the object,
      // we only want to roll back the transaction and propagate the exception. So, we carefully
      // pack the exception away into a value.
      txn->maybeRollback();
      return js.resolvedPromise(TxnResult{
        // TODO(cleanup): Simplify this once exception is passed using jsg::JsRef instead
        // of jsg::V8Ref
        jsg::JsValue(exception.getHandle(js)).addRef(js), true});
    });
  }).then(js, [](jsg::Lock& js, TxnResult result) -> jsg::JsRef<jsg::JsValue> {
    if (result.isError) {
      js.throwException(result.value.getHandle(js));
    } else {
      return kj::mv(result.value);
    }
  });
}

jsg::JsRef<jsg::JsValue> DurableObjectStorage::transactionSync(
    jsg::Lock& js, jsg::Function<jsg::JsRef<jsg::JsValue>()> callback) {
  KJ_IF_SOME(sqlite, cache->getSqliteDatabase()) {
    // SAVEPOINT is a readonly statement, but we need to trigger an outer TRANSACTION
    sqlite.notifyWrite();

    uint depth = transactionSyncDepth++;
    KJ_DEFER(--transactionSyncDepth);

    // TODO(perf): SQLite actually allows multiple savepoints with the same name. The name refers
    //   to the most-recent of these savepoints. This means we don't actually have to append the
    //   depth to each savepoint name like I originally thought. We should refactor this -- and use
    //   prepared statements.

    sqlite.run(SqliteDatabase::TRUSTED, kj::str("SAVEPOINT _cf_sync_savepoint_", depth));
    return js.tryCatch([&]() {
      auto result = callback(js);
      sqlite.run(SqliteDatabase::TRUSTED, kj::str("RELEASE _cf_sync_savepoint_", depth));
      return kj::mv(result);
    }, [&](jsg::Value exception) -> jsg::JsRef<jsg::JsValue> {
      sqlite.run(SqliteDatabase::TRUSTED, kj::str("ROLLBACK TO _cf_sync_savepoint_", depth));
      sqlite.run(SqliteDatabase::TRUSTED, kj::str("RELEASE _cf_sync_savepoint_", depth));
      js.throwException(kj::mv(exception));
    });
  } else {
    JSG_FAIL_REQUIRE(Error, "Durable Object is not backed by SQL.");
  }
}

jsg::Promise<void> DurableObjectStorage::sync(jsg::Lock& js) {
  KJ_IF_SOME(p, cache->onNoPendingFlush()) {
    // Note that we're not actually flushing since that will happen anyway once we go async. We're
    // merely checking if we have any pending or in-flight operations, and providing a promise that
    // resolves when they succeed. This promise only covers operations that were scheduled before
    // this method was invoked. If the cache has to flush again later from future operations, this
    // promise will resolve before they complete. If this promise were to reject, then the actor's
    // output gate will be broken first and the isolate will not resume synchronous execution.

    auto& context = IoContext::current();
    return context.awaitIo(js, kj::mv(p));
  } else {
    return js.resolvedPromise();
  }
}

SqliteDatabase& DurableObjectStorage::getSqliteDb(jsg::Lock& js) {
  KJ_IF_SOME(db, cache->getSqliteDatabase()) {
    // Actor is SQLite-backed but let's make sure SQL is configured to be enabled.
    if (enableSql) {
      return db;
    } else if (FeatureFlags::get(js).getWorkerdExperimental()) {
      // For backwards-compatibility, if the `experimental` compat flag is on, enable SQL. This is
      // deprecated, though, so warn in this case.

      // TODO(soon): Uncomment this warning after the D1 simulator has been updated to use
      //   `enableSql`. Otherwise, people doing local dev against D1 may see the warning
      //   spuriously.

      // IoContext::current().logWarningOnce(
      //     "Enabling SQL API based on the 'experimental' flag, but this will stop working soon. "
      //     "Instead, please set `enableSql = true` in your workerd config for the DO namespace. "
      //     "If using wrangler, under `[[migrations]]` in wrangler.toml, change `new_classes` to "
      //     "`new_sqlite_classes`.");

      return db;
    } else {
      // We're presumably running local workerd, which always uses SQLite for DO storage, but we're
      // trying to simulate a non-SQLite DO namespace for testing purposes.
      JSG_FAIL_REQUIRE(Error,
          "SQL is not enabled for this Durable Object class. To enable it, set "
          "`enableSql = true` in your workerd config for the class. If using wrangler, "
          "under `[[migrations]]` in wrangler.toml, change `new_classes` to "
          "`new_sqlite_classes`. Note that this change cannot be made after the class is "
          "already deployed to production.");
    }
  } else {
    // We're in production (not local workerd) and this DO namespace is not backed by SQLite.
    JSG_FAIL_REQUIRE(Error,
        "This Durable Object is not backed by SQLite storage, so the SQL API is not available. "
        "SQL can be enabled on a new Durable Object class by using the `new_sqlite_classes` "
        "instead of `new_classes` under `[[migrations]]` in your wrangler.toml, but an "
        "already-deployed class cannot be converted to SQLite (except by deleting the existing "
        "data).");
  }
}

jsg::Ref<SqlStorage> DurableObjectStorage::getSql(jsg::Lock& js) {
  return jsg::alloc<SqlStorage>(JSG_THIS);
}

kj::Promise<kj::String> DurableObjectStorage::getCurrentBookmark() {
  return cache->getCurrentBookmark();
}

kj::Promise<kj::String> DurableObjectStorage::getBookmarkForTime(kj::Date timestamp) {
  return cache->getBookmarkForTime(timestamp);
}

kj::Promise<kj::String> DurableObjectStorage::onNextSessionRestoreBookmark(kj::String bookmark) {
  return cache->onNextSessionRestoreBookmark(bookmark);
}

kj::Promise<void> DurableObjectStorage::waitForBookmark(kj::String bookmark) {
  return cache->waitForBookmark(bookmark);
}

void DurableObjectStorage::ensureReplicas() {
  return cache->ensureReplicas();
}

jsg::Optional<jsg::Ref<DurableObject>> DurableObjectStorage::getPrimary(jsg::Lock& js) {
  KJ_IF_SOME(primary, maybePrimary) {
    return primary.addRef();
  }
  return kj::none;
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
  getCache(OP_ROLLBACK);   // just for the checks
  KJ_IF_SOME(t, cacheTxn) {
    auto prom = t->rollback();
    IoContext::current().addWaitUntil(kj::mv(prom).attach(kj::mv(cacheTxn)));
    cacheTxn = kj::none;
  }
  rolledBack = true;
}

kj::Promise<void> DurableObjectTransaction::maybeCommit() {
  // cacheTxn is null if rollback() was called, in which case we don't want to commit anything.
  KJ_IF_SOME(t, cacheTxn) {
    auto maybePromise = t->commit();
    cacheTxn = kj::none;
    KJ_IF_SOME(promise, maybePromise) {
      return kj::mv(promise);
    }
  }
  return kj::READY_NOW;
}

void DurableObjectTransaction::maybeRollback() {
  cacheTxn = kj::none;
  rolledBack = true;
}

ActorState::ActorState(Worker::Actor::Id actorId,
    kj::Maybe<jsg::JsRef<jsg::JsValue>> transient,
    kj::Maybe<jsg::Ref<DurableObjectStorage>> persistent)
    : id(kj::mv(actorId)),
      transient(kj::mv(transient)),
      persistent(kj::mv(persistent)) {}

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

DurableObjectState::DurableObjectState(
    Worker::Actor::Id actorId, kj::Maybe<jsg::Ref<DurableObjectStorage>> storage)
    : id(kj::mv(actorId)),
      storage(kj::mv(storage)) {}

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

jsg::Promise<jsg::JsRef<jsg::JsValue>> DurableObjectState::blockConcurrencyWhile(
    jsg::Lock& js, jsg::Function<jsg::Promise<jsg::JsRef<jsg::JsValue>>()> callback) {
  return IoContext::current().blockConcurrencyWhile(js, kj::mv(callback));
}

void DurableObjectState::abort(jsg::Optional<kj::String> reason) {
  kj::String description = kj::mv(reason)
                               .map([](kj::String&& text) {
    return kj::str("broken.outputGateBroken; jsg.Error: ", text);
  }).orDefault([]() {
    return kj::str("broken.outputGateBroken; jsg.Error: Application called abort() to reset "
                   "Durable Object.");
  });

  kj::Exception error(kj::Exception::Type::FAILED, __FILE__, __LINE__, kj::mv(description));

  KJ_IF_SOME(s, storage) {
    // Make sure we _synchronously_ break storage so that there's no chance our promise fulfilling
    // will race against the output gate, possibly allowing writes to complete before being
    // canceled.
    s.get()->getActorCacheInterface().shutdown(error);
  }

  IoContext::current().abort(kj::cp(error));
  kj::throwFatalException(kj::mv(error));
}

Worker::Actor::HibernationManager& DurableObjectState::maybeInitHibernationManager(
    Worker::Actor& actor) {
  if (actor.getHibernationManager() == kj::none) {
    // If there's no hibernation manager created yet, we should create one.
    actor.setHibernationManager(kj::refcounted<HibernationManagerImpl>(
        actor.getLoopback(), KJ_REQUIRE_NONNULL(actor.getHibernationEventType())));
  }
  return KJ_REQUIRE_NONNULL(actor.getHibernationManager());
}

void DurableObjectState::acceptWebSocket(
    jsg::Ref<WebSocket> ws, jsg::Optional<kj::Array<kj::String>> tags) {
  JSG_ASSERT(!ws->isAccepted(), Error,
      "Cannot call `acceptWebSocket()` if the WebSocket was already accepted via `accept()`");
  JSG_ASSERT(ws->peerIsAwaitingCoupling(), Error,
      "Cannot call `acceptWebSocket()` on this WebSocket because its pair has already been "
      "accepted or used in a Response.");

  // We need to get a HibernationManager to give the websocket to.
  auto& a = KJ_REQUIRE_NONNULL(IoContext::current().getActor());
  // HibernationManager's acceptWebSocket() will throw if the websocket is in an incompatible state.
  // Note that not providing a tag is equivalent to providing an empty tag array.
  // Any duplicate tags will be ignored.
  kj::Array<kj::String> distinctTags = [&]() -> kj::Array<kj::String> {
    KJ_IF_SOME(t, tags) {
      kj::HashSet<kj::String> seen;
      size_t distinctTagCount = 0;
      for (auto tag = t.begin(); tag < t.end(); tag++) {
        JSG_REQUIRE(distinctTagCount < MAX_TAGS_PER_CONNECTION, Error,
            "a Hibernatable WebSocket cannot have more than ", MAX_TAGS_PER_CONNECTION, " tags");
        JSG_REQUIRE(tag->size() <= MAX_TAG_LENGTH, Error, "\"", *tag, "\" ",
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
  maybeInitHibernationManager(a).acceptWebSocket(kj::mv(ws), distinctTags);
}

kj::Array<jsg::Ref<api::WebSocket>> DurableObjectState::getWebSockets(
    jsg::Lock& js, jsg::Optional<kj::String> tag) {
  auto& a = KJ_REQUIRE_NONNULL(IoContext::current().getActor());
  KJ_IF_SOME(manager, a.getHibernationManager()) {
    return manager.getWebSockets(js, tag.map([](kj::StringPtr t) { return t; })).releaseAsArray();
  }
  return kj::Array<jsg::Ref<api::WebSocket>>();
}

void DurableObjectState::setWebSocketAutoResponse(
    jsg::Optional<jsg::Ref<WebSocketRequestResponsePair>> maybeReqResp) {
  auto& a = KJ_REQUIRE_NONNULL(IoContext::current().getActor());

  if (maybeReqResp == kj::none) {
    // If there's no request/response pair, we unset any current set auto response configuration.
    KJ_IF_SOME(manager, a.getHibernationManager()) {
      // If there's no hibernation manager created yet, there's nothing to do here.
      manager.setWebSocketAutoResponse(kj::none, kj::none);
    }
    return;
  }

  auto reqResp = KJ_REQUIRE_NONNULL(kj::mv(maybeReqResp));
  auto maxRequestOrResponseSize = 2048;

  JSG_REQUIRE(reqResp->getRequest().size() <= maxRequestOrResponseSize, RangeError,
      kj::str("Request cannot be larger than ", maxRequestOrResponseSize, " bytes. ",
          "A request of size ", reqResp->getRequest().size(), " was provided."));

  JSG_REQUIRE(reqResp->getResponse().size() <= maxRequestOrResponseSize, RangeError,
      kj::str("Response cannot be larger than ", maxRequestOrResponseSize, " bytes. ",
          "A response of size ", reqResp->getResponse().size(), " was provided."));

  maybeInitHibernationManager(a).setWebSocketAutoResponse(
      reqResp->getRequest(), reqResp->getResponse());
}

kj::Maybe<jsg::Ref<api::WebSocketRequestResponsePair>> DurableObjectState::
    getWebSocketAutoResponse() {
  auto& a = KJ_REQUIRE_NONNULL(IoContext::current().getActor());
  KJ_IF_SOME(manager, a.getHibernationManager()) {
    // If there's no hibernation manager created yet, there's nothing to do here.
    return manager.getWebSocketAutoResponse();
  }
  return kj::none;
}

kj::Maybe<kj::Date> DurableObjectState::getWebSocketAutoResponseTimestamp(jsg::Ref<WebSocket> ws) {
  return ws->getAutoResponseTimestamp();
}

void DurableObjectState::setHibernatableWebSocketEventTimeout(jsg::Optional<uint32_t> timeoutMs) {
  auto& a = KJ_REQUIRE_NONNULL(IoContext::current().getActor());

  // Setting a timeout = 0ms or an empty value will unset any currently set event timeout.
  // If there's no hibernation manager instantiated, we can skip the event timeout unsetting.
  if (timeoutMs == kj::none || KJ_REQUIRE_NONNULL(timeoutMs) == 0) {
    KJ_IF_SOME(hibernationManager, a.getHibernationManager()) {
      hibernationManager.setEventTimeout(kj::none);
    }
    return;
  }

  auto t = timeoutMs.orDefault((uint32_t)0);

  // We want to limit the duration of an event to a maximum of 7 days (604800 * 1000 millis).
  JSG_REQUIRE(t <= 604800 * 1000, Error, "Event timeout should not exceed 604800000 ms.");

  maybeInitHibernationManager(a).setEventTimeout(t);
}

kj::Maybe<uint32_t> DurableObjectState::getHibernatableWebSocketEventTimeout() {
  KJ_IF_SOME(a, IoContext::current().getActor()) {
    KJ_IF_SOME(manager, a.getHibernationManager()) {
      return manager.getEventTimeout();
    }
  }
  return kj::none;
}

kj::Array<kj::StringPtr> DurableObjectState::getTags(jsg::Lock& js, jsg::Ref<api::WebSocket> ws) {
  return ws->getHibernatableTags();
}

kj::Array<kj::byte> serializeV8Value(jsg::Lock& js, const jsg::JsValue& value) {
  jsg::Serializer serializer(js,
      jsg::Serializer::Options{
        .version = 15,
        .omitHeader = false,
      });
  serializer.write(js, value);
  auto released = serializer.release();
  return kj::mv(released.data);
}

jsg::JsValue deserializeV8Value(
    jsg::Lock& js, kj::ArrayPtr<const char> key, kj::ArrayPtr<const kj::byte> buf) {

  KJ_ASSERT(buf.size() > 0, "unexpectedly empty value buffer", key);
  try {
    // The js.tryCatch will handle the normal exception path. We wrap this in an
    // additional try/catch in case the js.tryCatch hits an exception that is
    // terminal for the isolate, causing exception to be rethrown, in which case
    // we throw a kj::Exception wrapping a jsg.Error.
    return js.tryCatch([&]() -> jsg::JsValue {
      jsg::Deserializer::Options options{};
      if (buf[0] != 0xFF) {
        // When Durable Objects was first released, it did not properly write headers when serializing
        // to storage. If we find that the header is missing (as indicated by the first byte not being
        // 0xFF), it's safe to assume that the data was written at the only serialization version we
        // used during that early time period, so we explicitly set that version here.
        options.version = 13;
        options.readHeader = false;
      }

      jsg::Deserializer deserializer(js, buf, kj::none, kj::none, options);

      return deserializer.readValue(js);
    }, [&](jsg::Value&& exception) mutable -> jsg::JsValue {
      // If we do hit a deserialization error, we log information that will be helpful in
      // understanding the problem but that won't leak too much about the customer's data. We
      // include the key (to help find the data in the database if it hasn't been deleted), the
      // length of the value, and the first three bytes of the value (which is just the v8-internal
      // version header and the tag that indicates the type of the value, but not its contents).
      kj::String actorId = getCurrentActorId().orDefault([]() { return kj::String(); });
      KJ_FAIL_ASSERT("actor storage deserialization failed", "failed to deserialize stored value",
          actorId, exception.getHandle(js), key, buf.size(),
          buf.first(std::min(static_cast<size_t>(3), buf.size())));
    });
  } catch (jsg::JsExceptionThrown&) {
    // We can occasionally hit an isolate termination here -- we prefix the error with jsg to avoid
    // counting it against our internal storage error metrics but also throw a KJ exception rather
    // than a jsExceptionThrown error to avoid confusing the normal termination handling code.
    // We don't expect users to ever actually see this error.
    JSG_FAIL_REQUIRE(Error,
        "isolate terminated while deserializing value from Durable Object "
        "storage; contact us if you're wondering why you're seeing this");
  }
}

}  // namespace workerd::api
