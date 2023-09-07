// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-cache.h"
#include <kj/test.h>
#include <kj/debug.h>
#include <capnp/dynamic.h>
#include <kj/list.h>
#include "io-gate.h"
#include <kj/thread.h>
#include <kj/source-location.h>
#include <workerd/util/capnp-mock.h>

namespace workerd {
namespace {

// =======================================================================================
// Test helpers specific to ActorCache test.

template <typename T>
kj::Promise<T> eagerlyReportExceptions(kj::Promise<T> promise, kj::SourceLocation location = {}) {
  // TODO(cleanup): Move to KJ somewhere?
  return promise.eagerlyEvaluate([location](kj::Exception&& e) -> T {
    KJ_LOG_AT(ERROR, location, e);
    kj::throwFatalException(kj::mv(e));
  });
}

template <typename T>
kj::Promise<T> expectUncached(kj::OneOf<T, kj::Promise<T>> result,
                              kj::SourceLocation location = {}) {
  // Expect that a result returned by get()/list()/delete() was not served entirely from cache,
  // and return the promise.
  KJ_SWITCH_ONEOF(result) {
    KJ_CASE_ONEOF(promise, kj::Promise<T>) {
      return eagerlyReportExceptions(kj::mv(promise), location);
    }
    KJ_CASE_ONEOF(value, T) {
      KJ_FAIL_ASSERT_AT(location, "result was unexpectedly cached");
    }
  }
  KJ_UNREACHABLE;
}

template <typename T>
T expectCached(kj::OneOf<T, kj::Promise<T>> result, kj::SourceLocation location = {}) {
  // Expect that a result returned by get()/list()/delete() was served entirely from cache, and
  // return that.
  KJ_SWITCH_ONEOF(result) {
    KJ_CASE_ONEOF(promise, kj::Promise<T>) {
      KJ_FAIL_ASSERT_AT(location, "result was unexpectedly uncached");
    }
    KJ_CASE_ONEOF(value, T) {
      return kj::mv(value);
    }
  }
  KJ_UNREACHABLE;
}

struct KeyValuePtr {
  kj::StringPtr key;
  kj::StringPtr value;

  inline bool operator==(const KeyValuePtr& other) const {
    return key == other.key && value == other.value;
  }
  inline kj::String toString() const {
    return kj::str(key, ": ", value);
  }
};

struct KeyValue {
  kj::String key;
  kj::String value;

  inline bool operator==(const KeyValuePtr& other) const {
    return key == other.key && value == other.value;
  }
  inline bool operator==(const KeyValue& other) const {
    return key == other.key && value == other.value;
  }
  inline kj::String toString() const {
    return kj::str(key, ": ", value);
  }
};

kj::ArrayPtr<const KeyValuePtr> kvs(kj::ArrayPtr<const KeyValuePtr> a) { return a; }
// We want to be able to write checks like:
//
//    KJ_ASSERT(results == {{"bar", "456"}, {"foo", "123"}});
//
// Unfortunately, the compiler is not smart enough to figure out how to interpret the braced list
// the right of the comparison. So, we give it a little help by wrapping it in `kvs()`, like:
//
//    KJ_ASSERT(results == kvs({{"bar", "456"}, {"foo", "123"}}));

// stringifyValues() is a convenience function that turns byte-array values returned by ActorCache
// into strings, for a variety of different return types.

kj::String stringifyValues(ActorCache::ValuePtr value) {
  return kj::str(value.asChars());
}
KeyValue stringifyValues(ActorCache::KeyValuePtrPair kv) {
  return { kj::str(kv.key), stringifyValues(kv.value) };
}
kj::Array<KeyValue> stringifyValues(const ActorCache::GetResultList& list) {
  return KJ_MAP(e, list) { return stringifyValues(e); };
}

template <typename T>
auto stringifyValues(const kj::Maybe<T>& values) {
  return values.map([](auto& v) { return stringifyValues(v); });
}

template <typename T>
kj::OneOf<decltype(stringifyValues(kj::instance<T>())),
          kj::Promise<decltype(stringifyValues(kj::instance<T>()))>>
    stringifyValues(kj::OneOf<T, kj::Promise<T>> result) {
  KJ_SWITCH_ONEOF(result) {
    KJ_CASE_ONEOF(promise, kj::Promise<T>) {
      return promise.then([](T result) { return stringifyValues(kj::mv(result)); });
    }
    KJ_CASE_ONEOF(value, T) {
      return stringifyValues(kj::mv(value));
    }
  }
  KJ_UNREACHABLE;
}

struct ActorCacheConvenienceWrappers {
  // Convenience methods to make test more concise by handling value conversions to/from strings,
  // and allowing parameters to be string literals instead of owned strings.
  //
  // This is formulated as a mixin inherited by ActorCacheTest, below, so that it can be reused
  // for transactions as well.

  ActorCacheConvenienceWrappers(ActorCacheOps& target): target(target) {}

  auto get(kj::StringPtr key, ActorCache::ReadOptions options = {}) {
    return stringifyValues(target.get(kj::str(key), options));
  }
  auto get(kj::ArrayPtr<const kj::StringPtr> keys, ActorCache::ReadOptions options = {}) {
    return stringifyValues(target.get(KJ_MAP(k, keys) { return kj::str(k); }, options));
  }
  auto getAlarm(ActorCache::ReadOptions options = {}) {
    return target.getAlarm(options);
  }

  auto list(kj::StringPtr begin, kj::StringPtr end,
            kj::Maybe<uint> limit = kj::none, ActorCache::ReadOptions options = {}) {
    return stringifyValues(target.list(kj::str(begin), kj::str(end), limit, options));
  }
  auto listReverse(kj::StringPtr begin, kj::StringPtr end,
                   kj::Maybe<uint> limit = kj::none, ActorCache::ReadOptions options = {}) {
    return stringifyValues(target.listReverse(kj::str(begin), kj::str(end), limit, options));
  }

  auto list(kj::StringPtr begin, decltype(nullptr),
            kj::Maybe<uint> limit = kj::none, ActorCache::ReadOptions options = {}) {
    return stringifyValues(target.list(kj::str(begin), kj::none, limit, options));
  }
  auto listReverse(kj::StringPtr begin, decltype(nullptr),
                   kj::Maybe<uint> limit = kj::none, ActorCache::ReadOptions options = {}) {
    return stringifyValues(target.listReverse(kj::str(begin), kj::none, limit, options));
  }

  auto put(kj::StringPtr key, kj::StringPtr value, ActorCache::WriteOptions options = {}) {
    return target.put(kj::str(key), kj::heapArray(value.asBytes()), options);
  }
  auto put(kj::ArrayPtr<const KeyValuePtr> kvs, ActorCache::WriteOptions options = {}) {
    return target.put(KJ_MAP(kv, kvs) {
      return ActorCache::KeyValuePair { kj::str(kv.key), kj::heapArray(kv.value.asBytes()) };
    }, options);
  }
  auto setAlarm(kj::Maybe<kj::Date> newTime, ActorCache::WriteOptions options = {}) {
    return target.setAlarm(newTime, options);
  }

  auto delete_(kj::StringPtr key, ActorCache::WriteOptions options = {}) {
    return target.delete_(kj::str(key), options);
  }
  auto delete_(kj::ArrayPtr<const kj::StringPtr> keys, ActorCache::WriteOptions options = {}) {
    return target.delete_(KJ_MAP(k, keys) { return kj::str(k); }, options);
  }

private:
  ActorCacheOps& target;
};

struct ActorCacheTestOptions {
  bool monitorOutputGate = true;
  size_t softLimit = 512 * 1024;
  size_t hardLimit = 1024 * 1024;
  kj::Duration staleTimeout = 1 * kj::SECONDS;
  size_t dirtyListByteLimit = 64 * 1024;
  size_t maxKeysPerRpc = 128;
  bool noCache = false;
  bool neverFlush = false;
};

struct ActorCacheTest: public ActorCacheConvenienceWrappers {
  // Common test setup code and helpers used in many test cases.

  kj::EventLoop loop;
  kj::WaitScope ws;
  kj::Own<MockServer> mockStorage;

  ActorCache::SharedLru lru;
  OutputGate gate;
  ActorCache cache;

  kj::Promise<void> gateBrokenPromise;

  kj::UnwindDetector unwindDetector;

  ActorCacheTest(ActorCacheTestOptions options = {},
                 MockServer::Pair<rpc::ActorStorage::Stage> mockPair =
                 MockServer::make<rpc::ActorStorage::Stage>())
      : ActorCacheConvenienceWrappers(cache),
        ws(loop), mockStorage(kj::mv(mockPair.mock)),
        lru({options.softLimit, options.hardLimit,
             options.staleTimeout, options.dirtyListByteLimit, options.maxKeysPerRpc,
             options.noCache, options.neverFlush}),
        cache(kj::mv(mockPair.client), lru, gate),
        gateBrokenPromise(options.monitorOutputGate
            ? eagerlyReportExceptions(gate.onBroken())
            : kj::Promise<void>(kj::READY_NOW)) {}

  ~ActorCacheTest() noexcept(false) {
    cache.markPendingReadsAbsentForTest();

    // Make sure if the output gate has been broken, the exception was reported. This is important
    // to report errors thrown inside flush(), since those won't otherwise propagate into the test
    // body.
    gateBrokenPromise.poll(ws);

    if (!unwindDetector.isUnwinding()) {
      // On successful test completion, also check that there were no extra calls to the mock.
      mockStorage->expectNoActivity(ws);

      cache.verifyConsistencyForTest();
    }
  }
};

KJ_TEST("ActorCache single-key basics") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Get value that is present on disk.
  {
    auto promise = expectUncached(test.get("foo"));

    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "foo"))
        .thenReturn(CAPNP(value = "bar"));

    auto result = KJ_ASSERT_NONNULL(promise.wait(ws));
    KJ_EXPECT(result == "bar");
  }

  // Get value that is absent on disk.
  {
    auto promise = expectUncached(test.get("bar"));

    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "bar"))
        .thenReturn(CAPNP());

    auto result = promise.wait(ws);
    KJ_EXPECT(result == kj::none);
  }

  // Get cached.
  {
    auto result = KJ_ASSERT_NONNULL(expectCached(test.get("foo")));
    KJ_EXPECT(result == "bar");
  }
  {
    auto result = expectCached(test.get("bar"));
    KJ_EXPECT(result == kj::none);
  }

  // Overwrite with a put().
  {
    test.put("foo", "baz");

    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "baz")]))
        .thenReturn(CAPNP());
  }

  {
    auto result = KJ_ASSERT_NONNULL(expectCached(test.get("foo")));
    KJ_EXPECT(result == "baz");
  }

  {
    KJ_ASSERT(expectCached(test.delete_("foo")));

    mockStorage->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["foo"]))
        .thenReturn(CAPNP(numDeleted = 1));
  }

  {
    auto result = expectCached(test.get("foo"));
    KJ_EXPECT(result == kj::none);
  }
}

KJ_TEST("ActorCache multi-key basics") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    // Request four keys, but only return two. The others should be marked empty. Note we
    // intentionally make sure that, in alphabetical order, the keys alternate between present
    // and absent, with the last one being absent, for maximum code coverage.
    auto promise = expectUncached(test.get({"foo"_kj, "bar"_kj, "baz"_kj, "qux"_kj}));

    mockStorage->expectCall("getMultiple", ws)
        .withParams(CAPNP(keys = ["bar", "baz", "foo", "qux"]), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          // baz absent
                                          (key = "foo", value = "123"),
                                          // qux absent
                                          ]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).thenReturn(CAPNP());

    auto results = promise.wait(ws);
    KJ_ASSERT(results == kvs({{"bar", "456"}, {"foo", "123"}}));
  }

  {
    auto results = expectCached(test.get({"foo"_kj, "bar"_kj, "baz"_kj, "qux"_kj}));
    KJ_ASSERT(results == kvs({{"bar", "456"}, {"foo", "123"}}));
  }

  {
    test.put({{"foo", "321"}, {"bar", "654"}});

    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "321"),
                                     (key = "bar", value = "654")]))
        .thenReturn(CAPNP());
  }

  {
    auto results = expectCached(test.get({"foo"_kj, "bar"_kj}));
    KJ_ASSERT(results == kvs({{"bar", "654"}, {"foo", "321"}}));
  }

  {
    KJ_ASSERT(expectCached(test.delete_({"foo"_kj, "bar"_kj, "baz"_kj, "qux"_kj})) == 2);

    mockStorage->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["foo", "bar"]))
        .thenReturn(CAPNP(numDeleted = 2));
  }

  {
    auto results = expectCached(test.get({"foo"_kj, "bar"_kj}));
    KJ_ASSERT(results == kvs({}));
  }
}

// =======================================================================================

KJ_TEST("ActorCache more puts") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    test.put("foo", "bar");

    // Value is immediately in cache.
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "bar");

    auto inProgressFlush = mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "bar")]));

    // Still in cache during flush.
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "bar");

    kj::mv(inProgressFlush).thenReturn(CAPNP());
  }

  // Still in cache after transaction completion.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "bar");

  // Putting the exact same value is redundant, so doesn't do an RPC.
  {
    test.put("foo", "bar");
    mockStorage->expectNoActivity(ws);
  }

  // Putting a different value is not redundant.
  {
    test.put("foo", "baz");

    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "baz")]))
        .thenReturn(CAPNP());
  }

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "baz");
}

KJ_TEST("ActorCache more deletes") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.delete_("foo"));

    // Value is immediately in cache.
    KJ_ASSERT(expectCached(test.get("foo")) == kj::none);

    auto mockDelete = mockStorage->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["foo"]));

    // Still in cache during flush.
    KJ_ASSERT(!promise.poll(ws));
    KJ_ASSERT(expectCached(test.get("foo")) == kj::none);

    kj::mv(mockDelete).thenReturn(CAPNP(numDeleted = 1));

    // Delete call returned true due to numDeleted = 1.
    KJ_ASSERT(promise.wait(ws));
  }

  // Still in cache after transaction completion.
  KJ_ASSERT(expectCached(test.get("foo")) == kj::none);

  // Try a case where the key isn't on disk.
  {
    auto promise = expectUncached(test.delete_("bar"));

    mockStorage->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["bar"]))
        .thenReturn(CAPNP(numDeleted = 0));

    // Delete call returned false due to numDeleted = 0.
    KJ_ASSERT(!promise.wait(ws));
  }

  // Deleting an already-deleted key is redundant, so doesn't do an RPC.
  {
    KJ_ASSERT(!expectCached(test.delete_("foo")));
    KJ_ASSERT(!expectCached(test.delete_("bar")));

    mockStorage->expectNoActivity(ws);
  }

  // Putting over the deleted key is not redundant.
  {
    test.put("foo", "baz");

    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "baz")]))
        .thenReturn(CAPNP());
  }

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "baz");

  // Deleting it again is not redundant.
  {
    KJ_ASSERT(expectCached(test.delete_("foo")));

    mockStorage->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["foo"]))
        .thenReturn(CAPNP(numDeleted = 1));
  }

  KJ_ASSERT(expectCached(test.get("foo")) == kj::none);
}

KJ_TEST("ActorCache more multi-puts") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Create a scenario where we have several cached and uncached keys.
  // foo, bar = cached with values
  // baz, qux = cached as absent
  // corge, grault = not cached
  {
    auto promise = expectUncached(test.get({"foo", "bar", "baz", "qux"}));

    mockStorage->expectCall("getMultiple", ws)
        .withParams(CAPNP(keys = ["bar", "baz", "foo", "qux"]), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).thenReturn(CAPNP());

    auto results = promise.wait(ws);
    KJ_ASSERT(results == kvs({{"bar", "456"}, {"foo", "123"}}));
  }

  {
    test.put({{"foo", "321"}, {"bar", "456"}, {"baz", "654"}, {"corge", "987"}});

    // Values are immediately in cache.
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "321");
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "456");
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "654");
    KJ_ASSERT(expectCached(test.get("qux")) == nullptr);
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("corge"))) == "987");

    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "321"),
                                     // bar omitted because it was redundant
                                     (key = "baz", value = "654"),
                                     (key = "corge", value = "987")]))
        .thenReturn(CAPNP());
  }

  // Fetch everything again for good measure.
  {
    auto promise = expectUncached(test.get({
        "foo"_kj, "bar"_kj, "baz"_kj, "qux"_kj, "corge"_kj, "grault"_kj}));

    // Only "grault" is not cached.
    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "grault"))
        .thenReturn(CAPNP());

    auto results = promise.wait(ws);
    KJ_ASSERT(results == kvs({
      {"bar", "456"},
      {"baz", "654"},
      {"corge", "987"},
      {"foo", "321"},
    }));
  }
}

KJ_TEST("ActorCache more multi-deletes") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Create a scenario where we have several cached and uncached keys.
  // foo, bar = cached with values
  // baz, qux = cached as absent
  // corge, grault = not cached
  {
    auto promise = expectUncached(test.get({"foo", "bar", "baz", "qux"}));

    mockStorage->expectCall("getMultiple", ws)
        .withParams(CAPNP(keys = ["bar", "baz", "foo", "qux"]), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).thenReturn(CAPNP());

    auto results = promise.wait(ws);
    KJ_ASSERT(results == kvs({{"bar", "456"}, {"foo", "123"}}));
  }

  {
    auto promise = expectUncached(test.delete_({"bar"_kj, "qux"_kj, "corge"_kj, "grault"_kj}));

    // Values are immediately in cache.
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
    KJ_ASSERT(expectCached(test.get("bar")) == nullptr);
    KJ_ASSERT(expectCached(test.get("baz")) == nullptr);
    KJ_ASSERT(expectCached(test.get("qux")) == nullptr);
    KJ_ASSERT(expectCached(test.get("corge")) == nullptr);
    KJ_ASSERT(expectCached(test.get("grault")) == nullptr);

    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["corge", "grault"]))
        .thenReturn(CAPNP(numDeleted = 1));
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["bar"]))
        .thenReturn(CAPNP(numDeleted = 65382));  // count is ignored
    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);

    KJ_ASSERT(promise.wait(ws) == 2);
  }

  // Fetch everything again for good measure.
  {
    auto promise = expectUncached(test.get({
        "foo"_kj, "bar"_kj, "baz"_kj, "qux"_kj, "corge"_kj, "grault"_kj, "garply"_kj}));

    // Only "garply" is not cached.
    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "garply"))
        .thenReturn(CAPNP(value = "abcd"));

    auto results = promise.wait(ws);
    KJ_ASSERT(results == kvs({
      {"foo", "123"},
      {"garply", "abcd"}
    }));
  }
}

KJ_TEST("ActorCache batching due to maxKeysPerRpc") {
  ActorCacheTest test({.maxKeysPerRpc = 2});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Do 5 puts and 3 deletes and expect a transaction that is batched accordingly given we set the
  // batch size to 2.
  test.put({{"foo", "123"}, {"bar", "456"}, {"baz", "789"}});
  test.put("qux", "555");
  test.put("corge", "999");

  // Note that because we drop the returned promises from these deletes, they end up as "muted"
  // deletes, so the resulting batches don't have to match the original calls.
  test.delete_("grault");
  test.delete_({"garply"_kj, "waldo"_kj});

  // We keep these promises, so they should not be "muted". Specifically, "count4" should be its own
  // batch despite fitting in a batch with "count3" because it's a separate delete.
  auto deleteProm1 = expectUncached(test.delete_({"count1"_kj, "count2"_kj, "count3"_kj}));
  auto deleteProm2 = expectUncached(test.delete_({"count4"_kj}));
  auto deleteProm3 = expectUncached(test.delete_({"count5"_kj, "count6"_kj}));

  auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
  mockTxn->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["count1", "count2"]))
      .thenReturn(CAPNP(numDeleted = 1)); // Treat one of this batch as present, 2 total.
  mockTxn->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["count3"]))
      .thenReturn(CAPNP(numDeleted = 1)); // Treat one of this batch as present, 2 total.
  mockTxn->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["count4"]))
      .thenReturn(CAPNP(numDeleted = 0)); // Treat this batch as absent.
  mockTxn->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["count5", "count6"]))
      .thenReturn(CAPNP(numDeleted = 2)); // Treat all of this batch as present.
  mockTxn->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["grault", "garply"]))
      .thenReturn(CAPNP(numDeleted = 1));
  mockTxn->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["waldo"]))
      .thenReturn(CAPNP(numDeleted = 1));
  mockTxn->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "foo", value = "123"),
                                    (key = "bar", value = "456")]))
      .thenReturn(CAPNP());
  mockTxn->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "baz", value = "789"),
                                    (key = "qux", value = "555")]))
      .thenReturn(CAPNP());
  mockTxn->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "corge", value = "999")]))
      .thenReturn(CAPNP());
  mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
  mockTxn->expectDropped(ws);

  KJ_EXPECT(deleteProm1.wait(ws) == 2);
  KJ_EXPECT(deleteProm2.wait(ws) == 0);
  KJ_EXPECT(deleteProm3.wait(ws) == 2);
}

KJ_TEST("ActorCache batching due to max storage RPC words") {
  ActorCacheTest test({.hardLimit = 128 * 1024 * 1024});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Doing 128 puts with 128 KiB values should exceed the 16 MiB limit enforced on storage RPCs.
  auto bigVal = kj::heapArray<const byte>(128 * 1024);
  for (int i = 0; i < 128; ++i) {
    test.cache.put(kj::str(i),
        kj::Array<const byte>(bigVal.begin(), bigVal.size(), kj::NullArrayDisposer::instance),
        ActorCache::WriteOptions());
  }

  auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
  mockTxn->expectCall("put", ws)
      .thenReturn(CAPNP());
  mockTxn->expectCall("put", ws)
      .thenReturn(CAPNP());
  mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
  mockTxn->expectDropped(ws);
}

KJ_TEST("ActorCache deleteAll()") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Populate the cache with some stuff.
  {
    auto promise = expectUncached(test.get({"qux"_kj, "corge"_kj}));

    mockStorage->expectCall("getMultiple", ws)
        .withParams(CAPNP(keys = ["corge", "qux"]), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "corge", value = "555")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).thenReturn(CAPNP());

    auto results = promise.wait(ws);
    KJ_ASSERT(results == kvs({{"corge", "555"}}));
  }

  test.put("foo", "123");  // plain put
  auto deletePromise = expectUncached(test.delete_({"bar"_kj, "baz"_kj, "grault"_kj}));
  test.put("baz", "789");  // overwrites a counted delete
  test.delete_("garply");  // uncounted delete

  auto deleteAll = test.cache.deleteAll({});

  // Post-deleteAll writes.
  test.put("grault", "12345");
  test.put("garply", "54321");
  test.put("waldo", "99999");

  // Alarms are not affected by deleteAll, so this alarm set should actually end up in
  // the pre-deleteAll flush.
  test.setAlarm(12345 * kj::MILLISECONDS + kj::UNIX_EPOCH);

  KJ_ASSERT(expectCached(test.get("foo")) == nullptr);
  KJ_ASSERT(expectCached(test.get("baz")) == nullptr);
  KJ_ASSERT(expectCached(test.get("corge")) == nullptr);
  KJ_ASSERT(expectCached(test.get("a")) == nullptr);
  KJ_ASSERT(expectCached(test.get("z")) == nullptr);
  KJ_ASSERT(expectCached(test.get("")) == nullptr);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("grault"))) == "12345");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("garply"))) == "54321");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("waldo"))) == "99999");

  {
    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["bar", "baz", "grault"]))
        .thenReturn(CAPNP(numDeleted = 2));
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["garply"]))
        .thenReturn(CAPNP(numDeleted = 2));
    mockTxn->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "123"),
                                     (key = "baz", value = "789")]))
        .thenReturn(CAPNP());
    mockTxn->expectCall("setAlarm", ws)
        .withParams(CAPNP(scheduledTimeMs = 12345))
        .thenReturn(CAPNP());
    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);
  }

  mockStorage->expectCall("deleteAll", ws)
      .thenReturn(CAPNP(numDeleted = 2));

  KJ_ASSERT(deleteAll.count.wait(ws) == 2);

  // Post-deleteAll writes in a new flush.
  {
    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "grault", value = "12345"),
                                     (key = "garply", value = "54321"),
                                     (key = "waldo", value = "99999")]))
        .thenReturn(CAPNP());
  }

  KJ_ASSERT(deletePromise.wait(ws) == 2);

  KJ_ASSERT(expectCached(test.get("foo")) == nullptr);
  KJ_ASSERT(expectCached(test.get("baz")) == nullptr);
  KJ_ASSERT(expectCached(test.get("corge")) == nullptr);
  KJ_ASSERT(expectCached(test.get("a")) == nullptr);
  KJ_ASSERT(expectCached(test.get("z")) == nullptr);
  KJ_ASSERT(expectCached(test.get("")) == nullptr);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("grault"))) == "12345");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("garply"))) == "54321");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("waldo"))) == "99999");
}

KJ_TEST("ActorCache deleteAll() during transaction commit") {
  // This tests a race condition that existed previously in the code.

  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Get a transaction going, and then issue a deleteAll() in the middle of it.
  test.put("foo", "123");

  {
    auto inProgressFlush = mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "123")]));

    // Issue a put and a deleteAll() here!
    test.put("bar", "456");
    test.cache.deleteAll({});

    kj::mv(inProgressFlush).thenReturn(CAPNP());
  }

  // We should see a new flush happen for the pre-deleteAll() write.
  {
    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "bar", value = "456")]))
        .thenReturn(CAPNP());
  }

  // Now the deleteAll() actually happens.
  mockStorage->expectCall("deleteAll", ws)
      .thenReturn(CAPNP());
}

KJ_TEST("ActorCache deleteAll() again when previous one isn't done yet") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Populate the cache with some stuff.
  {
    auto promise = expectUncached(test.get({"qux"_kj, "corge"_kj}));

    mockStorage->expectCall("getMultiple", ws)
        .withParams(CAPNP(keys = ["corge", "qux"]), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "corge", value = "555")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).thenReturn(CAPNP());

    auto results = promise.wait(ws);
    KJ_ASSERT(results == kvs({{"corge", "555"}}));
  }

  test.put("foo", "123");  // plain put
  auto deletePromise = expectUncached(test.delete_({"bar"_kj, "baz"_kj, "grault"_kj}));
  test.put("baz", "789");  // overwrites a counted delete
  test.delete_("garply");  // uncounted delete

  auto deleteAllA = test.cache.deleteAll({});

  // Post-deleteAll writes.
  test.put("grault", "12345");
  test.put("garply", "54321");
  test.put("waldo", "99999");

  {
    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["bar", "baz", "grault"]))
        .thenReturn(CAPNP(numDeleted = 2));
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["garply"]))
        .thenReturn(CAPNP(numDeleted = 2));
    mockTxn->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "123"),
                                     (key = "baz", value = "789")]))
        .thenReturn(CAPNP());
    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);
  }

  // Do another deleteAll() before the first one is done.
  auto deleteAllB = test.cache.deleteAll({});

  // And a write after that.
  test.put("fred", "2323");

  // Now finish it.
  mockStorage->expectCall("deleteAll", ws)
      .thenReturn(CAPNP(numDeleted = 2));
  KJ_ASSERT(deleteAllA.count.wait(ws) == 2);
  KJ_ASSERT(deleteAllB.count.wait(ws) == 0);

  // The deleteAll()s were coalesced, so only the final write is committed.
  {
    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "fred", value = "2323")]))
        .thenReturn(CAPNP());
  }
  KJ_ASSERT(deletePromise.wait(ws) == 2);
}

KJ_TEST("ActorCache coalescing") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Create a scenario where we have several cached and uncached keys.
  // foo, bar = cached with values
  // baz, qux = cached as absent
  // corge, grault, others = not cached
  {
    auto promise = expectUncached(test.get({"foo", "bar", "baz", "qux"}));

    mockStorage->expectCall("getMultiple", ws)
        .withParams(CAPNP(keys = ["bar", "baz", "foo", "qux"]), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).thenReturn(CAPNP());

    auto results = promise.wait(ws);
    KJ_ASSERT(results == kvs({{"bar", "456"}, {"foo", "123"}}));
  }

  // Now do several puts and deletes that overwrite each other, and make sure they coalesce
  // properly.
  {
    test.put({{"bar", "654"}, {"qux", "555"}, {"corge", "789"}});
    test.put("corge", "987");
    auto promise1 = expectUncached(test.delete_({"bar"_kj, "grault"_kj}));
    KJ_ASSERT(expectCached(test.delete_("foo")));
    auto promise2 = expectUncached(test.delete_({"garply"_kj, "waldo"_kj, "fred"_kj}));

    // Note this final put undoes a delete. However, the delete was of a key not in cache, so it
    // still has to be performed in order to produce the deletion count.
    test.put("waldo", "odlaw");

    auto values = expectCached(test.get({
        "foo"_kj, "bar"_kj, "baz"_kj, "qux"_kj, "corge"_kj, "grault"_kj,
        "garply"_kj, "waldo"_kj, "fred"_kj}));
    KJ_EXPECT(values ==  kvs({
      {"corge", "987"},
      {"qux", "555"},
      {"waldo", "odlaw"},
    }));

    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["grault"]))
        .thenReturn(CAPNP(numDeleted = 0));
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["garply", "waldo", "fred"]))
        .thenReturn(CAPNP(numDeleted = 2));
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["bar", "foo"]))
        .thenReturn(CAPNP(numDeleted = 65382));  // count is ignored
    mockTxn->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "qux", value = "555"),
                                     (key = "corge", value = "987"),
                                     (key = "waldo", value = "odlaw")]))
        .thenReturn(CAPNP());
    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);

    KJ_ASSERT(promise1.wait(ws) == 1);
    KJ_ASSERT(promise2.wait(ws) == 2);
  }
}

KJ_TEST("ActorCache canceled deletes are coalesced") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // A bunch of deletes where we immediately drop the returned promises.
  (void)expectUncached(test.delete_("foo"));
  (void)expectUncached(test.delete_({"bar"_kj, "baz"_kj}));
  (void)expectUncached(test.delete_("qux"));

  // Keep one promise.
  auto promise = expectUncached(test.delete_("corge"));

  // Overwrite one of them.
  test.put("qux", "blah");

  // The deletes where the caller stopped listening will be coalesced into one, or dropped entirely
  // if overwritten by a later put().
  {
    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["corge"]))
        .thenReturn(CAPNP(numDeleted = 0));
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["foo", "bar", "baz"]))
        .thenReturn(CAPNP(numDeleted = 1234));  // count ignored
    mockTxn->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "qux", value = "blah")]))
        .thenReturn(CAPNP());
    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);
  }

  KJ_ASSERT(!promise.wait(ws));
}

KJ_TEST("ActorCache get-put ordering") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Initiate a get, followed by a put and a delete that affect the same keys. Since the get()
  // started first, its final results later should not reflect the put and delete.
  auto promise1 = expectUncached(test.get({"foo"_kj, "bar"_kj, "baz"_kj}));
  test.put("foo", "123");
  auto deletePromise = expectUncached(test.delete_("bar"));

  // Verify cache content.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(expectCached(test.get("bar")) == nullptr);

  // Start another get. This time, "foo" and "bar" will be served from cache, but "baz" is still
  // on disk. This means this get won't complete immediately. We'll then overwrite the value of
  // "bar", but hope that the get() has already picked up the cached value for consistency.
  auto promise2 = expectUncached(test.get({"foo"_kj, "bar"_kj, "baz"_kj}));
  test.put("bar", "456");

  // Verify cache content.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "456");

  // Expect to receive the storage gets. But, don't return from them yet!
  KJ_ASSERT(!promise1.poll(ws));
  KJ_ASSERT(!promise2.poll(ws));
  mockStorage->expectCall("getMultiple", ws)
      .withParams(CAPNP(keys = ["bar", "baz", "foo"]), "stream"_kj)
      .useCallback("stream", [&](MockClient stream) {
    stream.call("values", CAPNP(list = [(key = "bar", value = "654"),
                                        (key = "baz", value = "987"),
                                        (key = "foo", value = "321")]))
        .expectReturns(CAPNP(), ws);
    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).thenReturn(CAPNP());

  // Next up, the flush transaction proceeds.
  auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
  mockTxn->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["bar"]))
      .thenReturn(CAPNP(numDeleted = 1));
  mockTxn->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "foo", value = "123"),
                                   (key = "bar", value = "456")]))
      .thenReturn(CAPNP());
  mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
  mockTxn->expectDropped(ws);


  // This returns exactly what came off disk, not reflecting any later writes.
  KJ_ASSERT(promise1.wait(ws) == kvs({{"bar", "654"}, {"baz", "987"}, {"foo", "321"}}));

  // The completed read returns cached results as of when it was called, merged with what it
  // read from disk.
  KJ_ASSERT(promise2.wait(ws) == kvs({{"baz", "987"}, {"foo", "123"}}));

  // The completed read brought "baz" into cache but didn't change "foo" or "bar".
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "456");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "987");


  // The completed read didn't mess with the cache.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "456");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "987");

  // Our delete finally finished.
  KJ_ASSERT(deletePromise.wait(ws) == 1);

  // Cache is still good.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "456");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "987");
}

KJ_TEST("ActorCache put during flush") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  test.put({{"foo", "123"}, {"bar", "456"}});

  {
    auto inProgressFlush = mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "123"),
                                    (key = "bar", value = "456")]));

    // We're in the middle of flushing... do a put. Should be fine.
    test.put("bar", "654");

    kj::mv(inProgressFlush).thenReturn(CAPNP());
  }

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "654");

  {
    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "bar", value = "654")]))
        .thenReturn(CAPNP());
  }

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "654");
}

KJ_TEST("ActorCache flush retry") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  test.put({{"foo", "123"}, {"bar", "456"}, {"baz", "789"}});
  auto promise1 = expectUncached(test.delete_({"qux"_kj, "quux"_kj}));
  auto promise2 = expectUncached(test.delete_({"corge"_kj, "grault"_kj}));

  {
    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    // One delete succeeds, the other throws (later).
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["qux", "quux"]))
        .thenReturn(CAPNP(numDeleted = 1));
    auto mockDelete = mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["corge", "grault"]));
    mockTxn->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "123"),
                                     (key = "bar", value = "456"),
                                     (key = "baz", value = "789")]))
        .thenReturn(CAPNP());

    // While the transaction is outstanding, some more puts and deletes mess with things...
    test.put("bar", "654");
    KJ_ASSERT(expectCached(test.delete_("baz")));
    test.put("qux", "987");
    test.put("corge", "555");

    kj::mv(mockDelete).thenThrow(KJ_EXCEPTION(DISCONNECTED, "delete failed"));
    mockTxn->expectCall("commit", ws).thenThrow(KJ_EXCEPTION(DISCONNECTED, "flush failed"));
    mockTxn->expectDropped(ws);
  }

  // Verify cache.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "654");
  KJ_ASSERT(expectCached(test.get("baz")) == nullptr);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("qux"))) == "987");
  KJ_ASSERT(expectCached(test.get("quux")) == nullptr);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("corge"))) == "555");
  KJ_ASSERT(expectCached(test.get("grault")) == nullptr);

  // The second delete had failed, though, so is still outstanding.
  KJ_ASSERT(!promise2.poll(ws));

  // The transaction will be retried, with the updated puts and deletes.
  {
    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    // Note that "corge" is still the subject of a delete, even though it has since been
    // overwritten by a put, because we still need to count the delete. "qux", on the other
    // hand, no longer needs counting, and has also been overwritten by a put(), so it doesn't
    // need to be deleted anymore. "quux" is still deleted, even though the count was returned
    // last time, because it hasn't been further overwritten, and that delete from last time
    // wasn't actually committed.
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["corge", "grault"]))
        .thenReturn(CAPNP(numDeleted = 2));
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["baz"]))
        .thenReturn(CAPNP(numDeleted = 1234));  // count ignored
    mockTxn->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "123"),
                                     (key = "bar", value = "654"),
                                     (key = "qux", value = "987"),
                                     (key = "corge", value = "555")]))
        .thenReturn(CAPNP());
    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);
  }

  // Verify cache.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "654");
  KJ_ASSERT(expectCached(test.get("baz")) == nullptr);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("qux"))) == "987");
  KJ_ASSERT(expectCached(test.get("quux")) == nullptr);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("corge"))) == "555");
  KJ_ASSERT(expectCached(test.get("grault")) == nullptr);

  // Second delete finished this time.
  KJ_ASSERT(promise2.wait(ws) == 2);

  // Although the transaction didn't complete, the delete did, and so it resolves.
  KJ_ASSERT(promise1.wait(ws) == 1);
}

KJ_TEST("ActorCache output gate blocked during flush") {
  ActorCacheTest test({.monitorOutputGate = false});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Gate is currently not blocked.
  KJ_ASSERT(test.gate.wait().poll(ws));

  // Do a put.
  test.put("foo", "123");
  test.delete_("bar");

  // Now it is blocked.
  auto gatePromise = test.gate.wait();
  KJ_ASSERT(!gatePromise.poll(ws));

  // Complete the transaction.
  {
    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["bar"]))
        .thenReturn(CAPNP(numDeleted = 0));
    mockTxn->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "123")]))
        .thenReturn(CAPNP());
    auto commitCall = mockTxn->expectCall("commit", ws);

    // Still blocked until commit completes.
    KJ_ASSERT(!gatePromise.poll(ws));

    kj::mv(commitCall).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);
  }

  KJ_ASSERT(gatePromise.poll(ws));
  gatePromise.wait(ws);
}

KJ_TEST("ActorCache output gate bypass") {
  ActorCacheTest test({.monitorOutputGate = false});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Gate is currently not blocked.
  test.gate.wait().wait(ws);

  // Do a put.
  test.put("foo", "123", {.allowUnconfirmed = true});

  // Gate still isn't blocked, because we set `allowUnconfirmed`.
  test.gate.wait().wait(ws);

  // Complete the transaction.
  {
    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "123")]))
        .thenReturn(CAPNP());
  }

  test.gate.wait().wait(ws);
}

KJ_TEST("ActorCache output gate bypass on one put but not the next") {
  ActorCacheTest test({.monitorOutputGate = false});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Gate is currently not blocked.
  test.gate.wait().wait(ws);

  // Do two puts, only bypassing on the first. The net result should be that the output gate is
  // in effect.
  test.put("foo", "123", {.allowUnconfirmed = true});
  test.put("bar", "456");

  // Now it is blocked.
  auto gatePromise = test.gate.wait();
  KJ_ASSERT(!gatePromise.poll(ws));

  // Complete the transaction.
  {
    auto inProgressFlush = mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "123"), (key = "bar", value = "456")]));

    // Still blocked until the flush completes.
    KJ_ASSERT(!gatePromise.poll(ws));

    kj::mv(inProgressFlush).thenReturn(CAPNP());
  }

  KJ_ASSERT(gatePromise.poll(ws));
  gatePromise.wait(ws);
}

KJ_TEST("ActorCache flush hard failure") {
  ActorCacheTest test({.monitorOutputGate = false});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto promise = test.gate.onBroken();

  test.put("foo", "123");

  KJ_ASSERT(!promise.poll(ws));

  {
    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "123")]))
        .thenThrow(KJ_EXCEPTION(FAILED, "jsg.Error: flush failed hard"));
  }

  KJ_EXPECT_THROW_MESSAGE("broken.outputGateBroken; jsg.Error: flush failed hard", promise.wait(ws));

  // Futher writes won't even try to start any new transactions because the failure killed them all.
  test.put("bar", "456");
}

KJ_TEST("ActorCache flush hard failure with output gate bypass") {
  ActorCacheTest test({.monitorOutputGate = false});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto promise = test.gate.onBroken();

  test.put("foo", "123", {.allowUnconfirmed = true});

  // The output gate is not applied.
  test.gate.wait().wait(ws);
  KJ_ASSERT(!promise.poll(ws));

  {
    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "123")]))
        .thenThrow(KJ_EXCEPTION(FAILED, "jsg.Error: flush failed hard"));
  }

  // The failure was still propagated to the output gate.
  KJ_EXPECT_THROW_MESSAGE("flush failed hard", promise.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("flush failed hard", test.gate.wait().wait(ws));

  // Futher writes won't even try to start any new transactions because the failure killed them all.
  test.put("bar", "456");
}

KJ_TEST("ActorCache read retry") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto promise = expectUncached(test.get("foo"));
  test.put("bar", "456");
  test.delete_("baz");

  // Expect the get, but don't resolve yet.
  auto mockGet = mockStorage->expectCall("get", ws)
      .withParams(CAPNP(key = "foo"));

  // Fail out the read with a disconnect.
  kj::mv(mockGet).thenThrow(KJ_EXCEPTION(DISCONNECTED, "read failed"));

  // It will be retried.
  auto mockGet2 = mockStorage->expectCall("get", ws)
      .withParams(CAPNP(key = "foo"));

  // Finish it.
  kj::mv(mockGet2).thenReturn(CAPNP(value = "123"));

  // Now the transaction starts actually writing (and completes).
  auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
  mockTxn->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["baz"]))
      .thenReturn(CAPNP(numDeleted = 0));
  mockTxn->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "bar", value = "456")]))
      .thenReturn(CAPNP());
  mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
  mockTxn->expectDropped(ws);

  // And the read finishes.
  KJ_ASSERT(KJ_ASSERT_NONNULL(promise.wait(ws)) == "123");
}

KJ_TEST("ActorCache read retry on flush containing only puts") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto promise = expectUncached(test.get("foo"));
  test.put("bar", "456");

  // Expect the get, but don't resolve yet.
  auto mockGet = mockStorage->expectCall("get", ws)
      .withParams(CAPNP(key = "foo"));

  // No activity on the flush yet (not even starting a txn), because reads are outstanding.
  mockStorage->expectNoActivity(ws);

  // Fail out the read with a disconnect.
  kj::mv(mockGet).thenThrow(KJ_EXCEPTION(DISCONNECTED, "read failed"));

  // It will be retried.
  auto mockGet2 = mockStorage->expectCall("get", ws)
      .withParams(CAPNP(key = "foo"));

  // Still no transaction activity.
  mockStorage->expectNoActivity(ws);

  // Finish it.
  kj::mv(mockGet2).thenReturn(CAPNP(value = "123"));

  // Now the transaction starts actually writing (and completes).
  mockStorage->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "bar", value = "456")]))
      .thenReturn(CAPNP());

  // And the read finishes.
  KJ_ASSERT(KJ_ASSERT_NONNULL(promise.wait(ws)) == "123");
}

// KJ_TEST("ActorCache read hard fail") {
//   ActorCacheTest test;
//   auto& ws = test.ws;
//   auto& mockStorage = test.mockStorage;

//   // Don't use expectCached() this time because we don't want eagerlyReportExceptions(), because
//   // we actually expect an exception.
//   auto promise = test.get("foo").get<kj::Promise<kj::Maybe<kj::String>>>();
//   test.put("bar", "456");
//   test.delete_("baz");

//   // Expect the get, but don't resolve yet.
//   auto mockGet = mockStorage->expectCall("get", ws)
//       .withParams(CAPNP(key = "foo"));

//   // Fail out the read with non-disconnect.
//   kj::mv(mockGet).thenThrow(KJ_EXCEPTION(FAILED, "read failed"));

//   // The read propagates the error.
//   KJ_EXPECT_THROW_MESSAGE("read failed", promise.wait(ws));

//   // The read is NOT retried, so expect the transaction to run now.
//   auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
//   mockTxn->expectCall("delete", ws)
//       .withParams(CAPNP(keys = ["baz"]))
//       .thenReturn(CAPNP(numDeleted = 0));
//   mockTxn->expectCall("put", ws)
//       .withParams(CAPNP(entries = [(key = "bar", value = "456")]))
//       .thenReturn(CAPNP());
//   mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
//   mockTxn->expectDropped(ws);

//   // The read is NOT retried.
//   mockStorage->expectNoActivity(ws);
// }

KJ_TEST("ActorCache read cancel") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  expectUncached(test.get("corge"));
  auto promise = expectUncached(test.get("foo"));
  test.put("bar", "456");
  test.delete_("baz");

  // Expect the get, but cancel the promise before we finish it.
  mockStorage->expectCall("getMultiple", ws)
      .withParams(CAPNP(keys = ["corge", "foo"]), "stream"_kj)
      .useCallback("stream", [&ws, promise = kj::mv(promise)](MockClient stream) mutable {
    promise = nullptr;
    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).thenReturn(CAPNP());

  // The transaction proceeds.
  auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
  mockTxn->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["baz"]))
      .thenReturn(CAPNP(numDeleted = 0));
  mockTxn->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "bar", value = "456")]))
      .thenReturn(CAPNP());

  mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
  mockTxn->expectDropped(ws);

  // Since we once asked for these keys, they are now cached even though we dropped the promises.
  KJ_EXPECT(expectCached(test.get("corge")) == kj::none);
  KJ_EXPECT(expectCached(test.get("foo")) == kj::none);
}

KJ_TEST("ActorCache read overwrite") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Make some gets but overwrite them in the cache with puts.
  auto promise1 = expectUncached(test.get("foo"));
  auto promise2 = expectUncached(test.get("bar"));
  expectUncached(test.get("baz"));

  test.put("foo", "456");
  test.put("bar", "789");
  test.put("baz", "123");

  // Since we still have the promise for foo and bar, we do send a get for them. But baz is not in
  // the map and has no waiters, so we don't bother.
  mockStorage->expectCall("getMultiple", ws)
      .withParams(CAPNP(keys = ["foo", "bar"]), "stream"_kj)
      .useCallback("stream", [&ws, promise = kj::mv(promise2)](MockClient stream) mutable {
    // Cancel the read for bar while we're flushing.
    promise = nullptr;
    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).thenReturn(CAPNP());

  // We've already replaced our dirty entries, so we don't see the previous value of foo.
  KJ_EXPECT(expectCached(test.get("foo")).orDefault({}) == "456");
  KJ_EXPECT(expectCached(test.get("bar")).orDefault({}) == "789");
  KJ_EXPECT(expectCached(test.get("baz")).orDefault({}) == "123");

  // The put proceeds.
  mockStorage->expectCall("put", ws)
      .withParams(CAPNP(entries = [
          (key = "foo", value = "456"),
          (key = "bar", value = "789"),
          (key = "baz", value = "123")]))
      .thenReturn(CAPNP());

  // Our values are now clean but nothing changes about the cached state.
  KJ_EXPECT(expectCached(test.get("foo")).orDefault({}) == "456");
  KJ_EXPECT(expectCached(test.get("bar")).orDefault({}) == "789");
  KJ_EXPECT(expectCached(test.get("baz")).orDefault({}) == "123");

  // We saw the previously absent value even though we were overwritten.
  auto val = promise1.wait(ws);
  KJ_EXPECT(val == kj::none);
}

KJ_TEST("ActorCache get-multiple multiple blocks") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto promise = expectUncached(test.get({"foo"_kj, "bar"_kj, "baz"_kj, "qux"_kj, "corge"_kj}));

  mockStorage->expectCall("getMultiple", ws)
      .withParams(CAPNP(keys = ["bar", "baz", "corge", "foo", "qux"]), "stream"_kj)
      .useCallback("stream", [&](MockClient stream) {
    stream.call("values", CAPNP(list = [(key = "baz", value = "456")]))
        .expectReturns(CAPNP(), ws);

    // At this point, "bar" and "baz" are considered cached.
    KJ_ASSERT(expectCached(test.get("bar")) == nullptr);
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "456");
    (void)expectUncached(test.get("corge"));
    (void)expectUncached(test.get("foo"));
    (void)expectUncached(test.get("qux"));

    stream.call("values", CAPNP(list = [(key = "foo", value = "789")]))
        .expectReturns(CAPNP(), ws);

    // At this point, everything except "qux" is cached.
    KJ_ASSERT(expectCached(test.get("bar")) == nullptr);
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "456");
    KJ_ASSERT(expectCached(test.get("corge")) == nullptr);
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "789");
    (void)expectUncached(test.get("qux"));

    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);

    // Now it's all cached.
    KJ_ASSERT(expectCached(test.get("bar")) == nullptr);
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "456");
    KJ_ASSERT(expectCached(test.get("corge")) == nullptr);
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "789");
    KJ_ASSERT(expectCached(test.get("qux")) == nullptr);
  }).thenReturn(CAPNP());

  KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "456"}, {"foo", "789"}}));
}

KJ_TEST("ActorCache get-multiple partial retry") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto promise = expectUncached(test.get({"foo"_kj, "bar"_kj, "baz"_kj, "qux"_kj}));

  mockStorage->expectCall("getMultiple", ws)
      .withParams(CAPNP(keys = ["bar", "baz", "foo", "qux"]), "stream"_kj)
      .useCallback("stream", [&](MockClient stream) {
    stream.call("values", CAPNP(list = [(key = "baz", value = "456")]))
        .expectReturns(CAPNP(), ws);
  }).thenThrow(KJ_EXCEPTION(DISCONNECTED, "read failed"));

  ws.poll();

  mockStorage->expectCall("getMultiple", ws)
      // Since "baz" was received, the caller knows that it only has to retry keys after that.
      .withParams(CAPNP(keys = ["foo", "qux"]), "stream"_kj)
      .useCallback("stream", [&](MockClient stream) {
    stream.call("values", CAPNP(list = [(key = "qux", value = "789")]))
        .expectReturns(CAPNP(), ws);
    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).thenReturn(CAPNP());

  // KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "456"}, {"qux", "789"}}));
}

// =======================================================================================
// OK... time for hard mode. Let's test list().

KJ_TEST("ActorCache list()") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "789"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));
  }

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "456");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "789");

  // Stuff in range that wasn't reported is cached as absent.
  KJ_ASSERT(expectCached(test.get("bara")) == nullptr);
  KJ_ASSERT(expectCached(test.get("corge")) == nullptr);
  KJ_ASSERT(expectCached(test.get("quw")) == nullptr);

  // Listing the same range again is fully cached.
  KJ_ASSERT(expectCached(test.list("bar", "qux")) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));

  // Limits can be applied to the cached results.
  KJ_ASSERT(expectCached(test.list("bar", "qux", 0u)) == kvs({}));
  KJ_ASSERT(expectCached(test.list("bar", "qux", 1)) ==
      kvs({{"bar", "456"}}));
  KJ_ASSERT(expectCached(test.list("bar", "qux", 2)) ==
      kvs({{"bar", "456"}, {"baz", "789"}}));
  KJ_ASSERT(expectCached(test.list("bar", "qux", 3)) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));
  KJ_ASSERT(expectCached(test.list("bar", "qux", 4)) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));
  KJ_ASSERT(expectCached(test.list("bar", "qux", 1000)) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));

  // The endpoint of the list is not cached.
  {
    auto promise = expectUncached(test.get("qux"));

    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "qux"))
        .thenReturn(CAPNP(value = "555"));

    auto result = KJ_ASSERT_NONNULL(promise.wait(ws));
    KJ_EXPECT(result == "555");
  }
}

KJ_TEST("ActorCache list() all") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list(nullptr, nullptr));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "789"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));
  }

  KJ_ASSERT(expectCached(test.get("")) == nullptr);
  KJ_ASSERT(expectCached(test.list(nullptr, nullptr)) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));
  KJ_ASSERT(expectCached(test.list("bar", "qux")) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));
  KJ_ASSERT(expectCached(test.list(nullptr, nullptr)) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));
  KJ_ASSERT(expectCached(test.list("baz", nullptr)) ==
      kvs({{"baz", "789"}, {"foo", "123"}}));
  KJ_ASSERT(expectCached(test.list(nullptr, "foo")) ==
      kvs({{"bar", "456"}, {"baz", "789"}}));
}

KJ_TEST("ActorCache list() with limit") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("bar", "qux", 3));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", limit = 3), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "789"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));
  }

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "456");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "789");

  // Stuff in range that wasn't reported is cached as absent -- but not past the last reported
  // value, which was "foo".
  KJ_ASSERT(expectCached(test.get("bara")) == nullptr);
  KJ_ASSERT(expectCached(test.get("corge")) == nullptr);
  KJ_ASSERT(expectCached(test.get("fon")) == nullptr);

  // Stuff after the last key is not in cache.
  (void)expectUncached(test.get("fooa"));

  // Listing the same range again, with the same limit or lower, is fully cached.
  KJ_ASSERT(expectCached(test.list("bar", "qux", 3)) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));
  KJ_ASSERT(expectCached(test.list("bar", "qux", 2)) ==
      kvs({{"bar", "456"}, {"baz", "789"}}));
  KJ_ASSERT(expectCached(test.list("bar", "qux", 1)) ==
      kvs({{"bar", "456"}}));
  KJ_ASSERT(expectCached(test.list("bar", "qux", 0u)) == kvs({}));

  // But a larger limit won't be cached.
  {
    auto promise = expectUncached(test.list("bar", "qux", 4));

    // The new list will start at "foo\0" with a limit of 1, so that it won't redundantly list foo
    // itself and will only get the one remaining key that it needs.
    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "foo\0", end = "qux", limit = 1), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "garply", value = "54321")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) ==
        kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}, {"garply", "54321"}}));
  }

  // Cached if we try it again though.
  KJ_ASSERT(expectCached(test.list("bar", "qux", 4)) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}, {"garply", "54321"}}));

  // Return our uncached get from earlier.
  mockStorage->expectCall("get", ws).withParams(CAPNP(key = "fooa")).thenReturn(CAPNP());
}

KJ_TEST("ActorCache list() with limit around negative entries") {
  // This checks for a bug where the initial scan through cache for list() applies the limit to
  // the total number of entries seen (positive or negative), when it really needs to apply only
  // to positive entries.

  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Set up a bunch of negative entries and a positive one after them.
  test.delete_({"bar1"_kj, "bar2"_kj, "bar3"_kj, "bar4"_kj});
  test.put("baz", "789");

  // Now do a list through them. It should see the positive entry in cache.
  {
    auto promise = expectUncached(test.list("bar", "qux", 3));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", limit = 7), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "bar1", value = "xxx"),
                                          (key = "bar3", value = "yyy"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux", 4)) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));

  // Acknowledge the transaction.
  {
    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("delete", ws).thenReturn(CAPNP());
    mockTxn->expectCall("put", ws).thenReturn(CAPNP());
    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);
  }
}

KJ_TEST("ActorCache list() start point is not present") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "789"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "789"}, {"foo", "123"}}));
  }

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");

  KJ_ASSERT(expectCached(test.get("bar")) == nullptr);
  KJ_ASSERT(expectCached(test.get("bara")) == nullptr);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "789");
  KJ_ASSERT(expectCached(test.get("baza")) == nullptr);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(expectCached(test.get("fooa")) == nullptr);
}

KJ_TEST("ActorCache list() multiple ranges") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("a", "c"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "a", end = "c"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "a", value = "1"),
                                          (key = "b", value = "2")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"a", "1"}, {"b", "2"}}));
  }

  KJ_ASSERT(expectCached(test.list("a", "c")) == kvs({{"a", "1"}, {"b", "2"}}));

  {
    auto promise = expectUncached(test.list("x", "z"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "x", end = "z"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "y", value = "9")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"y", "9"}}));
  }

  KJ_ASSERT(expectCached(test.list("a", "c")) == kvs({{"a", "1"}, {"b", "2"}}));
  KJ_ASSERT(expectCached(test.list("x", "z")) == kvs({{"y", "9"}}));

  (void)expectUncached(test.get("w"));
  (void)expectUncached(test.get("d"));
  (void)expectUncached(test.get("c"));
}

KJ_TEST("ActorCache list() with some already-cached keys in range") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Initialize cache with some clean entries, both positive and negative.
  {
    auto promise1 = expectUncached(test.get("bbb"));
    auto promise2 = expectUncached(test.get("ccc"));

    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "bbb"))
        .thenReturn(CAPNP());
    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "ccc"))
        .thenReturn(CAPNP(value = "cval"));

    KJ_ASSERT(promise1.wait(ws) == nullptr);
    KJ_ASSERT(KJ_ASSERT_NONNULL(promise2.wait(ws)) == "cval");
  }

  // Also some newly-written entries, positive and negative.
  test.put("ddd", "dval");
  auto deletePromise = expectUncached(test.delete_("eee"));

  // Now list the range. Explicitly produce results that contradict the recent writes.
  {
    auto promise = expectUncached(test.list("aaa", "fff"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "aaa", end = "fff"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "ccc", value = "cval"),
                                          (key = "eee", value = "eval")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"ccc", "cval"}, {"ddd", "dval"}}));
  }

  {
    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["eee"]))
        .thenReturn(CAPNP(numDeleted = 1));
    mockTxn->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "ddd", value = "dval")]))
        .thenReturn(CAPNP());
    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);
  }

  KJ_ASSERT(deletePromise.wait(ws) == 1);
}

KJ_TEST("ActorCache list() with seemingly-redundant dirty entries") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Write some stuff.
  auto deletePromise = expectUncached(test.delete_("bbb"));
  test.put("ccc", "cval");

  // Initiate a list operation, but don't complete it yet.
  auto listPromise = expectUncached(test.list("aaa", "fff"));
  auto listCall = mockStorage->expectCall("list", ws)
      .withParams(CAPNP(start = "aaa", end = "fff"), "stream"_kj);

  // Now write some contradictory values.
  test.put("bbb", "bval");
  KJ_ASSERT(expectCached(test.delete_("ccc")) == 1);

  // Now let the list complete in a way that matches what was just written.
  kj::mv(listCall).useCallback("stream", [&](MockClient stream) {
    stream.call("values", CAPNP(list = [(key = "bbb", value = "bval")]))
        .expectReturns(CAPNP(), ws);
    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).expectCanceled();

  // The list produces results consistent with when it started.
  KJ_ASSERT(listPromise.wait(ws) == kvs({{"ccc", "cval"}}));

  // But the later writes are still there in cache.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bbb"))) == "bval");
  KJ_ASSERT(expectCached(test.get("ccc")) == nullptr);

  // Now the transaction runs, notably containing only the original writes, not the later writes,
  // despite our flush being delayed by the reads.
  auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
  mockTxn->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["bbb"]))
      .thenReturn(CAPNP(numDeleted = 1));
  mockTxn->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "ccc", value = "cval")]))
      .thenReturn(CAPNP());
  mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
  mockTxn->expectDropped(ws);
  KJ_ASSERT(deletePromise.wait(ws) == 1);

  // And then there's a new transaction to write things back to the original values.
  // This is NOT REDUNDANT, even though the list results seemed to match the current cached values!
  // (I wrote this test to prove to myself that a DIRTY entry can't be marked CLEAN just because a
  // read result from disk came back with the same value.)
  mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
  mockTxn->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["ccc"]))
      .thenReturn(CAPNP(numDeleted = 1));
  mockTxn->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "bbb", value = "bval")]))
      .thenReturn(CAPNP());
  mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
  mockTxn->expectDropped(ws);

  // For good measure, verify list result can be served from cache.
  KJ_ASSERT(expectCached(test.list("aaa", "fff")) == kvs({{"bbb", "bval"}}));
}

KJ_TEST("ActorCache list() starting from known value") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    test.put("bar", "123");

    mockStorage->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "bar", value = "123")]))
      .thenReturn(CAPNP());
  }

  {
    auto promise = expectUncached(test.list("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar\0", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"bar", "123"}, {"baz", "456"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) == kvs({{"bar", "123"}, {"baz", "456"}}));
}

KJ_TEST("ActorCache list() starting from unknown value") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    test.put("baz", "456");

    mockStorage->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "baz", value = "456")]))
      .thenReturn(CAPNP());
  }

  {
    auto promise = expectUncached(test.list("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "456"}, {"foo", "123"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) == kvs({{"baz", "456"}, {"foo", "123"}}));
}

KJ_TEST("ActorCache list() consecutively, absent midpoint") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("bar", "corge"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "corge"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "456"}}));
  }

  {
    auto promise = expectUncached(test.list("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) == kvs({{"baz", "456"}, {"foo", "123"}}));
}

KJ_TEST("ActorCache list() consecutively reverse, absent midpoint") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}}));
  }

  {
    auto promise = expectUncached(test.list("bar", "corge"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "corge"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "456"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) == kvs({{"baz", "456"}, {"foo", "123"}}));
}

KJ_TEST("ActorCache list() consecutively, present midpoint") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("bar", "corge"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "corge"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "456"}}));
  }

  {
    auto promise = expectUncached(test.list("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "corge", value = "789"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"corge", "789"}, {"foo", "123"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) ==
      kvs({{"baz", "456"}, {"corge", "789"}, {"foo", "123"}}));
}

KJ_TEST("ActorCache list() consecutively reverse, present midpoint") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "corge", value = "789"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"corge", "789"}, {"foo", "123"}}));
  }

  {
    auto promise = expectUncached(test.list("bar", "corge"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "corge"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "456"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) ==
      kvs({{"baz", "456"}, {"corge", "789"}, {"foo", "123"}}));
}

KJ_TEST("ActorCache list() starting in known-empty gap") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Create a known-empty gap between "bar" and "corge".
  {
    auto promise = expectUncached(test.list("bar", "corge"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "corge"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({}));
  }

  // Now list from "baz" to "qux", which starts in the gap.
  {
    auto promise = expectUncached(test.list("baz", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) == kvs({{"foo", "123"}}));
}

KJ_TEST("ActorCache list() ending in known-empty gap") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Create a known-empty gap between "corge" and "qux".
  {
    auto promise = expectUncached(test.list("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({}));
  }

  // Now list from "bar" to "foo", which ends in the gap.
  {
    auto promise = expectUncached(test.list("bar", "foo"));

    // Note that the implementation of `list()` only looks for a prefix that it can skip, not a
    // suffix. Hence, the underlying list() call will go all the way to "foo", even though the
    // range from "qux" to "foo" is entirely in cache and hence in theory could be skipped. This
    // optimization is missing  because the code is complex enough already and it doesn't seem like
    // it would be a win that often.
    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "foo"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "123"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) == kvs({{"baz", "123"}}));
}

KJ_TEST("ActorCache list() with limit and dirty puts that end up past the limit") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  test.put("corge", "123");
  test.put("grault", "321");

  {
    auto promise = expectUncached(test.list("bar", "qux", 3));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", limit = 3), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "654"),
                                          (key = "foo", value = "789")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"bar", "456"}, {"baz", "654"}, {"corge", "123"}}));
  }

  // Although we only requested 3 results above, we actually listed through "foo" at least, so
  // now we can list 4 results and they'll all come from cache.
  KJ_ASSERT(expectCached(test.list("bar", "qux", 4)) ==
      kvs({{"bar", "456"}, {"baz", "654"}, {"corge", "123"}, {"foo", "789"}}));

  // Acknowledge the transaction.
  mockStorage->expectCall("put", ws).thenReturn(CAPNP());
}

KJ_TEST("ActorCache list() overwrite endpoint") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "corge", value = "789"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"corge", "789"}, {"foo", "123"}}));
  }

  test.put("qux", "456");

  KJ_ASSERT(expectCached(test.list("corge", "xyzzy", 3)) ==
      kvs({{"corge", "789"}, {"foo", "123"}, {"qux", "456"}}));

  // Acknowledge the transaction.
  mockStorage->expectCall("put", ws).thenReturn(CAPNP());
}

KJ_TEST("ActorCache list() delete endpoint") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "corge", value = "789"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"corge", "789"}, {"foo", "123"}}));
  }

  auto deletePromise = expectUncached(test.delete_("qux"));

  // Acknowledge the delete transaction.
  {
    mockStorage->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["qux"]))
      .thenReturn(CAPNP(numDeleted = 1));
  }

  KJ_ASSERT(deletePromise.wait(ws) == 1);

  // Do another list() through the deleted entry to make sure it didn't cause confusion. We apply
  // a limit to this list to check for a bug where negative entries in the fully-cached prefix
  // were incorrectly counted against the limit; only positive entries should be.
  {
    auto promise = expectUncached(test.list("corge", "xyzzy", 4));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "qux\0", end = "xyzzy", limit = 2), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "waldo", value = "555")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"corge", "789"}, {"foo", "123"}, {"waldo", "555"}}));
  }
}

KJ_TEST("ActorCache list() delete endpoint empty range") {
  // Same as last test except the listed range is totally empty.
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = []))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({}));
  }

  auto deletePromise = expectUncached(test.delete_("qux"));

  // Acknowledge the delete transaction.
  {
    mockStorage->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["qux"]))
      .thenReturn(CAPNP(numDeleted = 1));
  }

  KJ_ASSERT(deletePromise.wait(ws) == 1);

  // Do another list() through the deleted entry to make sure it didn't cause confusion. We apply
  // a limit to this list to check for a bug where negative entries in the fully-cached prefix
  // were incorrectly counted against the limit; only positive entries should be.
  {
    auto promise = expectUncached(test.list("corge", "xyzzy", 4));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "qux\0", end = "xyzzy", limit = 4), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "qux", value = "555")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({}));
  }
}

KJ_TEST("ActorCache list() interleave streaming with other ops") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto promise = expectUncached(test.list("bar", "qux"));
  kj::Promise<kj::Maybe<kj::String>> promise2 = nullptr;
  mockStorage->expectCall("list", ws)
      .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
      .useCallback("stream", [&](MockClient stream) {
    stream.call("values", CAPNP(list = [(key = "bar", value = "123"),
                                        (key = "corge", value = "456")]))
        .expectReturns(CAPNP(), ws);

    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "123");
    KJ_ASSERT(expectCached(test.get("baz")) == nullptr);
    promise2 = expectUncached(test.get("grault"));

    test.put("foo", "987");

    stream.call("values", CAPNP(list = [(key = "foo", value = "789"),
                                        (key = "garply", value = "555")]))
        .expectReturns(CAPNP(), ws);

    KJ_ASSERT(expectCached(test.delete_("garply")) == 1);

    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).expectCanceled();

  KJ_ASSERT(promise.wait(ws) ==
      kvs({{"bar", "123"}, {"corge", "456"}, {"foo", "789"}, {"garply", "555"}}));

  KJ_ASSERT(expectCached(test.list("bar", "qux")) ==
      kvs({{"bar", "123"}, {"corge", "456"}, {"foo", "987"}}));

  // There will be two flushes waiting since the put of "foo" will have started before the
  // delete of "garply"
  mockStorage->expectCall("get", ws)
      .withParams(CAPNP(key = "grault"))
      .thenReturn(CAPNP());
  mockStorage->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "foo", value = "987")]))
      .thenReturn(CAPNP());
  mockStorage->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["garply"]))
      .thenReturn(CAPNP());
  KJ_ASSERT(promise2.wait(ws) == nullptr);
}

KJ_TEST("ActorCache list() end of first block deleted at inopportune time") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Do a delete, wait for the commit... and then hold it open.
  auto deletePromise = expectUncached(test.delete_("corge"));

  auto mockDelete = mockStorage->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["corge"]));

  // Now do a list.
  auto promise = expectUncached(test.list("bar", "qux"));

  mockStorage->expectCall("list", ws)
      .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
      .useCallback("stream", [&](MockClient stream) {
    // First block ends at the deleted entry.
    stream.call("values", CAPNP(list = [(key = "bar", value = "123"),
                                        (key = "corge", value = "456")]))
        .expectReturns(CAPNP(), ws);

    // Let the delete finish. So now the last key in the first block is cached as a negative
    // clean entry.
    kj::mv(mockDelete).thenReturn(CAPNP());

    // Continue on.
    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).expectCanceled();

  KJ_ASSERT(promise.wait(ws) == kvs({{"bar", "123"}}));

  KJ_ASSERT(expectCached(test.list("bar", "qux")) == kvs({{"bar", "123"}}));
}

KJ_TEST("ActorCache list() retry on failure") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("bar", "qux", 4));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", limit = 4), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "789")]))
          .expectReturns(CAPNP(), ws);
    }).thenThrow(KJ_EXCEPTION(DISCONNECTED, "oops"));

    // Retry starts from `baz`.
    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "baz\0", end = "qux", limit = 2), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      // Duplicates of earlier keys will be ignored.
      stream.call("values", CAPNP(list = [(key = "bar", value = "IGNORE"),
                                          (key = "baz", value = "IGNORE"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));
}

KJ_TEST("ActorCache get() of endpoint of previous list() returning negative is cached correctly") {
  // This tests for a bug that once existed in ActorCache::addReadResultToCache() where we compared
  // against a moved-away value.
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("bar", "qux", 4));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", limit = 4), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({}));
  }

  {
    auto promise = expectUncached(test.get("qux"));
    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "qux"))
        .thenReturn(CAPNP());
    KJ_ASSERT(promise.wait(ws) == nullptr);
  }

  KJ_ASSERT(expectCached(test.get("qux")) == nullptr);
}

// =======================================================================================
// And now... listReverse()... needs all its own tests...

KJ_TEST("ActorCache listReverse()") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.listReverse("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123"),
                                          (key = "baz", value = "789"),
                                          (key = "bar", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));
  }

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "456");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "789");

  // Stuff in range that wasn't reported is cached as absent.
  KJ_ASSERT(expectCached(test.get("bara")) == nullptr);
  KJ_ASSERT(expectCached(test.get("corge")) == nullptr);
  KJ_ASSERT(expectCached(test.get("quw")) == nullptr);

  // Listing the same range again is fully cached.
  KJ_ASSERT(expectCached(test.listReverse("bar", "qux")) ==
      kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));

  // Limits can be applied to the cached results.
  KJ_ASSERT(expectCached(test.listReverse("bar", "qux", 0u)) == kvs({}));
  KJ_ASSERT(expectCached(test.listReverse("bar", "qux", 1)) ==
      kvs({{"foo", "123"}}));
  KJ_ASSERT(expectCached(test.listReverse("bar", "qux", 2)) ==
      kvs({{"foo", "123"}, {"baz", "789"}}));
  KJ_ASSERT(expectCached(test.listReverse("bar", "qux", 3)) ==
      kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));
  KJ_ASSERT(expectCached(test.listReverse("bar", "qux", 4)) ==
      kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));
  KJ_ASSERT(expectCached(test.listReverse("bar", "qux", 1000)) ==
      kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));

  // The endpoint of the list is not cached.
  {
    auto promise = expectUncached(test.get("qux"));

    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "qux"))
        .thenReturn(CAPNP(value = "555"));

    auto result = KJ_ASSERT_NONNULL(promise.wait(ws));
    KJ_EXPECT(result == "555");
  }
}

KJ_TEST("ActorCache listReverse() all") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.listReverse(nullptr, nullptr));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123"),
                                          (key = "baz", value = "789"),
                                          (key = "bar", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));
  }

  KJ_ASSERT(expectCached(test.get("")) == nullptr);
  KJ_ASSERT(expectCached(test.list(nullptr, nullptr)) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));
  KJ_ASSERT(expectCached(test.listReverse("bar", "qux")) ==
      kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));
  KJ_ASSERT(expectCached(test.listReverse(nullptr, nullptr)) ==
      kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));
  KJ_ASSERT(expectCached(test.listReverse("baz", nullptr)) ==
      kvs({{"foo", "123"}, {"baz", "789"}}));
  KJ_ASSERT(expectCached(test.listReverse(nullptr, "foo")) ==
      kvs({{"baz", "789"}, {"bar", "456"}}));
}

KJ_TEST("ActorCache listReverse() with limit") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.listReverse("abc", "qux", 3));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "abc", end = "qux", limit = 3, reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123"),
                                          (key = "baz", value = "789"),
                                          (key = "bar", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));
  }

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "456");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "789");

  // Stuff in range that wasn't reported is cached as absent -- but not past the last reported
  // value, which was "foo".
  KJ_ASSERT(expectCached(test.get("bara")) == nullptr);
  KJ_ASSERT(expectCached(test.get("corge")) == nullptr);
  KJ_ASSERT(expectCached(test.get("fon")) == nullptr);

  // Stuff before the first key is not in cache.
  (void)expectUncached(test.get("baq"));

  // Listing the same range again, with the same limit or lower, is fully cached.
  KJ_ASSERT(expectCached(test.listReverse("bar", "qux", 3)) ==
      kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));
  KJ_ASSERT(expectCached(test.listReverse("bar", "qux", 2)) ==
      kvs({{"foo", "123"}, {"baz", "789"}}));
  KJ_ASSERT(expectCached(test.listReverse("bar", "qux", 1)) ==
      kvs({{"foo", "123"}}));
  KJ_ASSERT(expectCached(test.listReverse("bar", "qux", 0u)) == kvs({}));

  // But a larger limit won't be cached.
  {
    auto promise = expectUncached(test.listReverse("abc", "qux", 4));

    // The new list will end at "bar" with a limit of 1, so that it will only get the one remaining
    // key that it needs.
    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "abc", end = "bar", limit = 1, reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baa", value = "xyz")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) ==
        kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}, {"baa", "xyz"}}));
  }

  // Cached if we try it again though.
  KJ_ASSERT(expectCached(test.listReverse("abc", "qux", 4)) ==
      kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}, {"baa", "xyz"}}));
}

KJ_TEST("ActorCache listReverse() with limit around negative entries") {
  // This checks for a bug where the initial scan through cache for list() applies the limit to
  // the total number of entries seen (positive or negative), when it really needs to apply only
  // to positive entries.

  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Set up a bunch of negative entries and a positive one after them.
  test.delete_({"bar1"_kj, "bar2"_kj, "bar3"_kj, "bar4"_kj});
  test.put("bar", "456");

  // Now do a list through them. It should see the positive entry in cache.
  {
    auto promise = expectUncached(test.listReverse("bar", "qux", 3));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", limit = 7, reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123"),
                                          (key = "baz", value = "789"),
                                          (key = "bar3", value = "yyy"),
                                          (key = "bar1", value = "xxx")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));
  }

  KJ_ASSERT(expectCached(test.listReverse("bar", "qux", 3)) ==
      kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));

  // Acknowledge the transaction.
  {
    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("delete", ws).thenReturn(CAPNP());
    mockTxn->expectCall("put", ws).thenReturn(CAPNP());
    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);
  }
}

KJ_TEST("ActorCache listReverse() start point is not present") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.listReverse("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123"),
                                          (key = "baz", value = "789")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}, {"baz", "789"}}));
  }

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");

  KJ_ASSERT(expectCached(test.get("bar")) == nullptr);
  KJ_ASSERT(expectCached(test.get("bara")) == nullptr);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "789");
  KJ_ASSERT(expectCached(test.get("baza")) == nullptr);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  KJ_ASSERT(expectCached(test.get("fooa")) == nullptr);
}

KJ_TEST("ActorCache listReverse() multiple ranges") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.listReverse("a", "c"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "a", end = "c", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "b", value = "2"),
                                          (key = "a", value = "1")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"b", "2"}, {"a", "1"}}));
  }

  KJ_ASSERT(expectCached(test.listReverse("a", "c")) == kvs({{"b", "2"}, {"a", "1"}}));

  {
    auto promise = expectUncached(test.listReverse("x", "z"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "x", end = "z", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "y", value = "9")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"y", "9"}}));
  }

  KJ_ASSERT(expectCached(test.listReverse("a", "c")) == kvs({{"b", "2"}, {"a", "1"}}));
  KJ_ASSERT(expectCached(test.listReverse("x", "z")) == kvs({{"y", "9"}}));

  (void)expectUncached(test.get("w"));
  (void)expectUncached(test.get("d"));
  (void)expectUncached(test.get("c"));
}

KJ_TEST("ActorCache listReverse() with some already-cached keys in range") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Initialize cache with some clean entries, both positive and negative.
  {
    auto promise1 = expectUncached(test.get("bbb"));
    auto promise2 = expectUncached(test.get("ccc"));

    mockStorage->expectCall("getMultiple", ws)
        .withParams(CAPNP(keys = ["bbb", "ccc"]), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "ccc", value = "cval")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).thenReturn(CAPNP());

    KJ_ASSERT(promise1.wait(ws) == nullptr);
    KJ_ASSERT(KJ_ASSERT_NONNULL(promise2.wait(ws)) == "cval");
  }

  // Also some newly-written entries, positive and negative.
  test.put("ddd", "dval");
  auto deletePromise = expectUncached(test.delete_("eee"));

  // Now list the range. Explicitly produce results that contradict the recent writes.
  {
    auto promise = expectUncached(test.listReverse("aaa", "fff"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "aaa", end = "fff", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "eee", value = "eval"),
                                          (key = "ccc", value = "cval")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"ddd", "dval"}, {"ccc", "cval"}}));
  }

  {
    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["eee"]))
        .thenReturn(CAPNP(numDeleted = 1));
    mockTxn->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "ddd", value = "dval")]))
        .thenReturn(CAPNP());
    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);
  }

  KJ_ASSERT(deletePromise.wait(ws) == 1);
}

KJ_TEST("ActorCache listReverse() with seemingly-redundant dirty entries") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Write some stuff.
  auto deletePromise = expectUncached(test.delete_("bbb"));
  test.put("ccc", "cval");

  // Initiate a list operation, but don't complete it yet.
  auto listPromise = expectUncached(test.listReverse("aaa", "fff"));
  auto listCall = mockStorage->expectCall("list", ws)
      .withParams(CAPNP(start = "aaa", end = "fff", reverse = true), "stream"_kj);

  // Now write some contradictory values.
  test.put("bbb", "bval");
  KJ_ASSERT(expectCached(test.delete_("ccc")) == 1);

  // Now let the list complete in a way that matches what was just written.
  kj::mv(listCall).useCallback("stream", [&](MockClient stream) {
    stream.call("values", CAPNP(list = [(key = "bbb", value = "bval")]))
        .expectReturns(CAPNP(), ws);
    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).expectCanceled();

  // The list produces results consistent with when it started.
  KJ_ASSERT(listPromise.wait(ws) == kvs({{"ccc", "cval"}}));

  // But the later writes are still there in cache.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bbb"))) == "bval");
  KJ_ASSERT(expectCached(test.get("ccc")) == nullptr);

  // The transaction completes now.
  auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
  mockTxn->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["bbb"]))
      .thenReturn(CAPNP(numDeleted = 1));
  mockTxn->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "ccc", value = "cval")]))
      .thenReturn(CAPNP());
  mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
  mockTxn->expectDropped(ws);
  KJ_ASSERT(deletePromise.wait(ws) == 1);

  // And then there's a new transaction to write things back to the original values.
  // This is NOT REDUNDANT, even though the list results seemed to match the current cached values!
  // (I wrote this test to prove to myself that a DIRTY entry can't be marked CLEAN just because a
  // read result from disk came back with the same value.)
  mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
  mockTxn->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["ccc"]))
      .thenReturn(CAPNP(numDeleted = 1));
  mockTxn->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "bbb", value = "bval")]))
      .thenReturn(CAPNP());
  mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
  mockTxn->expectDropped(ws);

  // For good measure, verify list result can be served from cache.
  KJ_ASSERT(expectCached(test.listReverse("aaa", "fff")) == kvs({{"bbb", "bval"}}));
}

KJ_TEST("ActorCache listReverse() starting from known value") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    test.put("bar", "123");

    mockStorage->expectCall("put", ws).thenReturn(CAPNP());
  }

  {
    auto promise = expectUncached(test.listReverse("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "456"}, {"bar", "123"}}));
  }

  KJ_ASSERT(expectCached(test.listReverse("bar", "qux")) == kvs({{"baz", "456"}, {"bar", "123"}}));
}

KJ_TEST("ActorCache listReverse() starting from unknown value") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    test.put("baz", "456");

    mockStorage->expectCall("put", ws).thenReturn(CAPNP());
  }

  {
    auto promise = expectUncached(test.listReverse("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123"),
                                          (key = "baz", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}, {"baz", "456"}}));
  }

  KJ_ASSERT(expectCached(test.listReverse("bar", "qux")) == kvs({{"foo", "123"}, {"baz", "456"}}));
}

KJ_TEST("ActorCache listReverse() consecutively, absent midpoint") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.listReverse("bar", "corge"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "corge", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "456"}}));
  }

  {
    auto promise = expectUncached(test.listReverse("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}}));
  }

  KJ_ASSERT(expectCached(test.listReverse("bar", "qux")) == kvs({{"foo", "123"}, {"baz", "456"}}));
}

KJ_TEST("ActorCache listReverse() consecutively reverse, absent midpoint") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.listReverse("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}}));
  }

  {
    auto promise = expectUncached(test.listReverse("bar", "corge"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "corge", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "456"}}));
  }

  KJ_ASSERT(expectCached(test.listReverse("bar", "qux")) == kvs({{"foo", "123"}, {"baz", "456"}}));
}

KJ_TEST("ActorCache listReverse() consecutively, present midpoint") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.listReverse("bar", "corge"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "corge", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "456"}}));
  }

  {
    auto promise = expectUncached(test.listReverse("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123"),
                                          (key = "corge", value = "789")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}, {"corge", "789"}}));
  }

  KJ_ASSERT(expectCached(test.listReverse("bar", "qux")) ==
      kvs({{"foo", "123"}, {"corge", "789"}, {"baz", "456"}}));
}

KJ_TEST("ActorCache listReverse() consecutively reverse, present midpoint") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.listReverse("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123"),
                                          (key = "corge", value = "789")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}, {"corge", "789"}}));
  }

  {
    auto promise = expectUncached(test.listReverse("bar", "corge"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "corge", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "456"}}));
  }

  KJ_ASSERT(expectCached(test.listReverse("bar", "qux")) ==
      kvs({{"foo", "123"}, {"corge", "789"}, {"baz", "456"}}));
}

KJ_TEST("ActorCache listReverse() starting in known-empty gap") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Create a known-empty gap between "bar" and "corge".
  {
    auto promise = expectUncached(test.list("bar", "corge"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "corge"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({}));
  }

  // Now list from "baz" to "qux", which starts in the gap.
  {
    auto promise = expectUncached(test.listReverse("baz", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "baz", end = "qux", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) == kvs({{"foo", "123"}}));
}

KJ_TEST("ActorCache listReverse() ending in known-empty gap") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Create a known-empty gap between "corge" and "qux".
  {
    auto promise = expectUncached(test.list("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({}));
  }

  // Now list from "bar" to "foo", which ends in the gap.
  {
    auto promise = expectUncached(test.listReverse("bar", "foo"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "corge", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "123"}}));
  }

  KJ_ASSERT(expectCached(test.listReverse("bar", "qux")) == kvs({{"baz", "123"}}));
}

KJ_TEST("ActorCache listReverse() with limit and dirty puts that end up past the limit") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  test.put("corge", "123");
  test.put("bar", "321");

  {
    auto promise = expectUncached(test.listReverse("bar", "qux", 3));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", limit = 3, reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "grault", value = "456"),
                                          (key = "foo", value = "654"),
                                          (key = "baz", value = "789")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"grault", "456"}, {"foo", "654"}, {"corge", "123"}}));
  }

  // Although we only requested 3 results above, we actually listed through "baz" at least, so
  // now we can list 4 results and they'll all come from cache.
  KJ_ASSERT(expectCached(test.listReverse("bar", "qux", 4)) ==
      kvs({{"grault", "456"}, {"foo", "654"}, {"corge", "123"}, {"baz", "789"}}));

  // Acknowledge the transaction.
  mockStorage->expectCall("put", ws).thenReturn(CAPNP());
}

KJ_TEST("ActorCache listReverse() overwrite endpoint") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.listReverse("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123"),
                                          (key = "corge", value = "789")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}, {"corge", "789"}}));
  }

  test.put("qux", "456");

  KJ_ASSERT(expectCached(test.list("corge", "xyzzy", 3)) ==
      kvs({{"corge", "789"}, {"foo", "123"}, {"qux", "456"}}));

  // Acknowledge the transaction.
  mockStorage->expectCall("put", ws).thenReturn(CAPNP());
}

KJ_TEST("ActorCache listReverse() delete endpoint") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.listReverse("corge", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "corge", end = "qux", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123"),
                                          (key = "corge", value = "789")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}, {"corge", "789"}}));
  }

  KJ_ASSERT(expectCached(test.delete_("corge")) == 1);

  // Acknowledge the delete transaction.
  {
    mockStorage->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["corge"]))
      .thenReturn(CAPNP(numDeleted = 1));
  }

  {
    auto promise = expectUncached(test.listReverse("bar", "qux", 4));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "corge", limit = 3, reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "555")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}, {"baz", "555"}}));
  }
}

KJ_TEST("ActorCache listReverse() interleave streaming with other ops") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto promise = expectUncached(test.listReverse("baa", "qux"));

  mockStorage->expectCall("list", ws)
      .withParams(CAPNP(start = "baa", end = "qux", reverse = true), "stream"_kj)
      .useCallback("stream", [&](MockClient stream) {
    stream.call("values", CAPNP(list = [(key = "garply", value = "555"),
                                        (key = "foo", value = "789")]))
        .expectReturns(CAPNP(), ws);

    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("garply"))) == "555");
    KJ_ASSERT(expectCached(test.get("grault")) == nullptr);
    KJ_ASSERT(expectCached(test.get("gah")) == nullptr);
    auto promise2 = expectUncached(test.get("baz"));
    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "baz"))
        .thenReturn(CAPNP());
    KJ_ASSERT(promise2.wait(ws) == nullptr);

    test.put("corge", "987");

    stream.call("values", CAPNP(list = [(key = "corge", value = "456"),
                                        (key = "bar", value = "123")]))
        .expectReturns(CAPNP(), ws);

    KJ_ASSERT(expectCached(test.delete_("bar")) == 1);

    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).expectCanceled();

  KJ_ASSERT(promise.wait(ws) ==
      kvs({{"garply", "555"}, {"foo", "789"}, {"corge", "456"}, {"bar", "123"}}));

  KJ_ASSERT(expectCached(test.listReverse("bar", "qux")) ==
      kvs({{"garply", "555"}, {"foo", "789"}, {"corge", "987"}}));

  // There will be two flushes waiting since the put of "foo" will have started before the
  // delete of "garply"
  {
    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "corge", value = "987")]))
        .thenReturn(CAPNP());
  }
  {
    mockStorage->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["bar"]))
        .thenReturn(CAPNP());
  }
}

KJ_TEST("ActorCache listReverse() end of first block deleted at inopportune time") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Do a delete, wait for the commit... and then hold it open.
  auto deletePromise = expectUncached(test.delete_("corge"));

  auto mockDelete = mockStorage->expectCall("delete", ws)
      .withParams(CAPNP(keys = ["corge"]));

  // Now do a list.
  auto promise = expectUncached(test.listReverse("bar", "qux"));

  mockStorage->expectCall("list", ws)
      .withParams(CAPNP(start = "bar", end = "qux", reverse = true), "stream"_kj)
      .useCallback("stream", [&](MockClient stream) {
    // First block ends at the deleted entry.
    stream.call("values", CAPNP(list = [(key = "foo", value = "456"),
                                        (key = "corge", value = "123")]))
        .expectReturns(CAPNP(), ws);

    // Let the delete finish. So now the last key in the first block is cached as a negative
    // clean entry.
    kj::mv(mockDelete).thenReturn(CAPNP());

    // Continue on.
    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).expectCanceled();

  KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "456"}}));

  KJ_ASSERT(expectCached(test.listReverse("bar", "qux")) == kvs({{"foo", "456"}}));
}

KJ_TEST("ActorCache listReverse() retry on failure") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.listReverse("bar", "qux", 4));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", limit = 4, reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123"),
                                          (key = "baz", value = "789")]))
          .expectReturns(CAPNP(), ws);
    }).thenThrow(KJ_EXCEPTION(DISCONNECTED, "oops"));

    // Retry starts from `baz`.
    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "baz", limit = 2, reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      // Duplicates of earlier keys will be ignored.
      stream.call("values", CAPNP(list = [(key = "foo", value = "IGNORE"),
                                          (key = "baz", value = "IGNORE"),
                                          (key = "bar", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));
  }

  KJ_ASSERT(expectCached(test.listReverse("bar", "qux")) ==
      kvs({{"foo", "123"}, {"baz", "789"}, {"bar", "456"}}));
}

// =======================================================================================
// LRU purge

constexpr size_t ENTRY_SIZE = 120;
KJ_TEST("ActorCache LRU purge") {
  ActorCacheTest test({.softLimit = 1 * ENTRY_SIZE});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto promise = expectUncached(test.get("foo"));
  mockStorage->expectCall("get", ws)
      .withParams(CAPNP(key = "foo"))
      .thenReturn(CAPNP(value = "123"));

  KJ_ASSERT(KJ_ASSERT_NONNULL(promise.wait(ws)) == "123");

  // Still cached.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");

  promise = expectUncached(test.get("bar"));
  mockStorage->expectCall("get", ws)
      .withParams(CAPNP(key = "bar"))
      .thenReturn(CAPNP(value = "456"));

  KJ_ASSERT(KJ_ASSERT_NONNULL(promise.wait(ws)) == "456");

  // Still cached.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == "456");

  // But foo was evicted.
  (void)expectUncached(test.get("foo"));
}

KJ_TEST("ActorCache LRU purge ordering") {
  ActorCacheTest test({.softLimit = 4 * ENTRY_SIZE});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  test.put("foo", "123");
  test.put("bar", "456");
  test.put("baz", "789");
  test.put("qux", "555");

  // Let the flush of the puts complete.
  mockStorage->expectCall("put", ws).thenReturn(CAPNP());

  // Ensure the flush actually completes (marking dirty entries as clean) before continuing.
  test.gate.wait().wait(ws);

  // Touch foo.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");

  // Write two new values to push things out.
  test.put("xxx", "aaa");
  test.put("yyy", "bbb");

  // More puts flushing.
  mockStorage->expectCall("put", ws).thenReturn(CAPNP());

  // Foo and qux live, bar and baz evicted.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");
  (void)expectUncached(test.get("bar"));
  (void)expectUncached(test.get("baz"));
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("qux"))) == "555");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("xxx"))) == "aaa");
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("yyy"))) == "bbb");
}

KJ_TEST("ActorCache LRU purge larger") {
  ActorCacheTest test({.softLimit = 32 * ENTRY_SIZE});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto kilobyte = kj::str(kj::repeat('x', 1024));

  auto promise = expectUncached(test.get("foo"));
  mockStorage->expectCall("get", ws)
      .withParams(CAPNP(key = "foo"))
      .thenReturn(CAPNP(value = "123"));

  KJ_ASSERT(KJ_ASSERT_NONNULL(promise.wait(ws)) == "123");

  test.put("bar", kilobyte);
  test.put("baz", kilobyte);
  test.put("qux", kilobyte);

  // Still cached.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");

  test.put("corge", kilobyte);

  // Dropped from cache, because the puts are in-flight and so cannot be dropped. This read gets
  // sent off before the puts above becase the event loop hasn't been yielded yet.
  // TODO(cleanup): We hold onto the promise here (even though in theory it'd be fine to drop)
  // because the capnp-mock framework doesn't handle dropped client promises well (capnp destructs
  // the ReceivedCall before waitForEvent resolves and hands control back to expectCall, leaving
  // receivedPromises empty in expectCall).
  promise = expectUncached(test.get("foo"));

  test.put("grault", kilobyte);
  test.put("garply", kilobyte);

  // Everything dirty is still in cache despite exceeding cache bounds.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("bar"))) == kilobyte);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == kilobyte);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("qux"))) == kilobyte);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("corge"))) == kilobyte);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("grault"))) == kilobyte);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("garply"))) == kilobyte);

  {
    // We have to wait for the get before the flush since capnp-mock doesn't continue waiting
    // after receiving the first call, and in this case the first call received will be the get.
    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "foo"))
        .thenReturn(CAPNP(value = "123"));
    mockStorage->expectCall("put", ws).thenReturn(CAPNP());
    // Ensure the flush actually completes (marking dirty entries as clean) before continuing.
    test.gate.wait().wait(ws);
  }

  (void)expectUncached(test.get("bar"));
  (void)expectUncached(test.get("baz"));
  (void)expectUncached(test.get("qux"));
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("corge"))) == kilobyte);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("grault"))) == kilobyte);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("garply"))) == kilobyte);
}

KJ_TEST("ActorCache LRU purge") {
  ActorCacheTest test({.softLimit = 1});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto promise = expectUncached(test.get({"foo"_kj, "bar"_kj, "baz"_kj}));
  mockStorage->expectCall("getMultiple", ws)
      .withParams(CAPNP(keys = ["bar", "baz", "foo"]), "stream"_kj)
      .useCallback("stream", [&](MockClient stream) {
    stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                        (key = "baz", value = "789"),
                                        (key = "foo", value = "123")]))
        .expectReturns(CAPNP(), ws);
    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).expectCanceled();

  KJ_ASSERT(promise.wait(ws) == kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "123"}}));

  // Nothing was cached, because nothing fit in the LRU.
  KJ_ASSERT(test.lru.currentSize() == 0);
  (void)expectUncached(test.get("foo"));
  (void)expectUncached(test.get("bar"));
  (void)expectUncached(test.get("baz"));
}

KJ_TEST("ActorCache evict on timeout") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto timePoint = kj::UNIX_EPOCH;
  KJ_ASSERT(test.cache.evictStale(timePoint) == kj::none);

  auto ackFlush = [&]() {
    mockStorage->expectCall("put", ws).thenReturn(CAPNP());
    // Ensure the flush actually completes (marking dirty entries as clean) before continuing.
    test.gate.wait().wait(ws);
  };

  test.put("foo", "123");
  test.put("bar", "456");
  ackFlush();

  KJ_ASSERT(test.cache.evictStale(timePoint + 100 * kj::MILLISECONDS) == kj::none);
  KJ_ASSERT(test.cache.evictStale(timePoint + 200 * kj::MILLISECONDS) == kj::none);
  KJ_ASSERT(test.cache.evictStale(timePoint + 500 * kj::MILLISECONDS) == kj::none);

  expectCached(test.get("foo"));
  expectCached(test.get("bar"));

  KJ_ASSERT(test.cache.evictStale(timePoint + 1000 * kj::MILLISECONDS) == nullptr);
  // foo and bar are now stale

  // add baz
  test.put("baz", "789");
  ackFlush();

  // don't check foo because we want it to be evicted, but touch bar
  expectCached(test.get("bar"));

  KJ_ASSERT(test.cache.evictStale(timePoint + 2000 * kj::MILLISECONDS) == nullptr);
  // Now foo should be evicted and bar and baz stale.

  // Verify foo is evicted.
  (void)expectUncached(test.get("foo"));

  // Touch bar.
  expectCached(test.get("bar"));

  KJ_ASSERT(test.cache.evictStale(timePoint + 3000 * kj::MILLISECONDS) == nullptr);
  // Now baz should have been evicted, but bar is still here because we keep touching it.

  expectCached(test.get("bar"));
  (void)expectUncached(test.get("baz"));
}

KJ_TEST("ActorCache backpressure due to dirtyPressureThreshold") {
  ActorCacheTest test({.dirtyListByteLimit = 2 * ENTRY_SIZE});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto timePoint = kj::UNIX_EPOCH;
  KJ_ASSERT(test.cache.evictStale(timePoint) == nullptr);

  KJ_ASSERT(test.put("foo", "123") == nullptr);
  KJ_ASSERT(test.put("bar", "456") == nullptr);
  auto promise1 = KJ_ASSERT_NONNULL(test.put("baz", "789"));
  auto promise2 = KJ_ASSERT_NONNULL(test.put("qux", "555"));

  // These deletes are actually cached, BUT backpressure will apply to make them return a promise.
  auto promise3 = expectUncached(test.delete_("baz"));
  auto promise4 = expectUncached(test.delete_({"qux"_kj}));

  // A delete of an unknown keys will also apply backpressure, of course.
  auto promise5 = expectUncached(test.delete_("corge"));
  auto promise6 = expectUncached(test.delete_({"grault"_kj}));

  // Let the write transaction complete.
  {
    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("delete", ws).thenReturn(CAPNP(numDeleted = 0));
    mockTxn->expectCall("delete", ws).thenReturn(CAPNP(numDeleted = 0));
    mockTxn->expectCall("delete", ws).thenReturn(CAPNP(numDeleted = 0));
    mockTxn->expectCall("put", ws).thenReturn(CAPNP());

    // Test for bogus `KJ_ASSERT(flushScheduled)` in `ActorCache::getBackpressure()`.
    auto promise7 = KJ_ASSERT_NONNULL(test.cache.evictStale(timePoint));

    KJ_ASSERT(!promise1.poll(ws));
    KJ_ASSERT(!promise2.poll(ws));
    KJ_ASSERT(!promise3.poll(ws));
    KJ_ASSERT(!promise4.poll(ws));
    KJ_ASSERT(!promise5.poll(ws));
    KJ_ASSERT(!promise6.poll(ws));
    KJ_ASSERT(!promise7.poll(ws));

    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());

    promise1.wait(ws);
    promise2.wait(ws);
    KJ_ASSERT(promise3.wait(ws));
    KJ_ASSERT(promise4.wait(ws) == 1);
    KJ_ASSERT(!promise5.wait(ws));
    KJ_ASSERT(promise6.wait(ws) == 0);
    promise7.wait(ws);

    mockTxn->expectDropped(ws);
  }
}

KJ_TEST("ActorCache lru evict entry with known-empty gaps") {
  ActorCacheTest test({.softLimit = 5 * ENTRY_SIZE});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Populate cache.
  {
    auto promise = expectUncached(test.list("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "789"),
                                          (key = "corge", value = "555"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) ==
        kvs({{"bar", "456"}, {"baz", "789"}, {"corge", "555"}, {"foo", "123"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"corge", "555"}, {"foo", "123"}}));

  // touch some stuff so that "corge" is the oldest entry.
  expectCached(test.list("foo", "qux"));
  expectCached(test.get("bar"));
  expectCached(test.get("baz"));

  // do a put() to force an eviction.
  {
    test.put("xyzzy", "x");

    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "xyzzy", value = "x")]))
        .thenReturn(CAPNP());
  }

  // The ranges before and after "corge" are missing, but everything else is still in cache.
  KJ_ASSERT(expectCached(test.list("bar", "baz")) == kvs({{"bar", "456"}}));
  KJ_ASSERT(expectCached(test.get("bay")) == nullptr);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "789");
  KJ_ASSERT(expectCached(test.list("foo", "qux")) == kvs({{"foo", "123"}}));
  KJ_ASSERT(expectCached(test.get("fooa")) == nullptr);

  (void)expectUncached(test.get("baza"));
  (void)expectUncached(test.get("corge"));
  (void)expectUncached(test.get("fo"));
}

KJ_TEST("ActorCache lru evict gap entry with known-empty gaps") {
  ActorCacheTest test({.softLimit = 5 * ENTRY_SIZE});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Populate cache.
  {
    auto promise = expectUncached(test.list("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "789"),
                                          (key = "corge", value = "555"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) ==
        kvs({{"bar", "456"}, {"baz", "789"}, {"corge", "555"}, {"foo", "123"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"corge", "555"}, {"foo", "123"}}));

  // touch some stuff so that "qux" is the oldest entry.
  expectCached(test.get("bar"));
  expectCached(test.get("baz"));
  expectCached(test.get("corge"));
  expectCached(test.get("foo"));

  // We still have a cached gap between "foo" and "qux".
  KJ_ASSERT(expectCached(test.get("foo+1")) == nullptr);

  // do a put() to force an eviction.
  {
    test.put("xyzzy", "x");

    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "xyzzy", value = "x")]))
        .thenReturn(CAPNP());
  }

  // Okay, that gap is gone now.
  (void)expectUncached(test.get("foo+1"));
}

KJ_TEST("ActorCache lru evict entry with trailing known-empty gap (followed by END_GAP)") {
  ActorCacheTest test({.softLimit = 5 * ENTRY_SIZE});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Populate cache.
  {
    auto promise = expectUncached(test.list("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "789"),
                                          (key = "corge", value = "555"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) ==
        kvs({{"bar", "456"}, {"baz", "789"}, {"corge", "555"}, {"foo", "123"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"corge", "555"}, {"foo", "123"}}));

  // touch some stuff so that "foo" is the oldest entry.
  expectCached(test.get("bar"));
  expectCached(test.get("baz"));
  expectCached(test.get("corge"));

  // do a put() to force an eviction.
  {
    test.put("xyzzy", "x");

    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "xyzzy", value = "x")]))
        .thenReturn(CAPNP());
  }

  // The range after "foo" is missing, but everything else is still in cache.
  KJ_ASSERT(expectCached(test.list("bar", "corge")) == kvs({{"bar", "456"}, {"baz", "789"}}));
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("corge"))) == "555");

  (void)expectUncached(test.get("corgf"));
  (void)expectUncached(test.get("foo"));
  (void)expectUncached(test.get("quw"));
  (void)expectUncached(test.get("qux"));
  (void)expectUncached(test.get("quy"));
}

KJ_TEST("ActorCache timeout entry with known-empty gaps") {
  ActorCacheTest test({.softLimit = 5 * ENTRY_SIZE});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto startTime = kj::UNIX_EPOCH;
  test.cache.evictStale(startTime);

  // Populate cache.
  {
    auto promise = expectUncached(test.list("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "789"),
                                          (key = "corge", value = "555"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) ==
        kvs({{"bar", "456"}, {"baz", "789"}, {"corge", "555"}, {"foo", "123"}}));
  }

  KJ_ASSERT(expectCached(test.list("bar", "qux")) ==
      kvs({{"bar", "456"}, {"baz", "789"}, {"corge", "555"}, {"foo", "123"}}));

  // Make all entries STALE.
  test.cache.evictStale(startTime + 1 * kj::SECONDS);

  // touch some stuff so that "corge" is the only STALE entry.
  expectCached(test.list("foo", "qux"));
  expectCached(test.get("bar"));
  expectCached(test.get("baz"));

  // Time out "corge".
  test.cache.evictStale(startTime + 2 * kj::SECONDS);

  // The ranges before and after "corge" are missing, but everything else is still in cache.
  KJ_ASSERT(expectCached(test.list("bar", "baz")) == kvs({{"bar", "456"}}));
  KJ_ASSERT(expectCached(test.get("bay")) == nullptr);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("baz"))) == "789");
  KJ_ASSERT(expectCached(test.list("foo", "qux")) == kvs({{"foo", "123"}}));
  KJ_ASSERT(expectCached(test.get("fooa")) == nullptr);

  (void)expectUncached(test.get("baza"));
  (void)expectUncached(test.get("corge"));
  (void)expectUncached(test.get("fo"));
}


KJ_TEST("ActorCache evictStale entire list with end marker") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto timePoint = kj::UNIX_EPOCH;
  KJ_ASSERT(test.cache.evictStale(timePoint) == nullptr);

  {
    // Populate a decent list.
    auto promise = expectUncached(test.list("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "789"),
                                          (key = "corge", value = "555"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) ==
        kvs({{"bar", "456"}, {"baz", "789"}, {"corge", "555"}, {"foo", "123"}}));
  }

  KJ_EXPECT(test.lru.currentSize() > 0);

  // First mark the entire cache as stale.
  timePoint += 1 * kj::SECONDS;
  KJ_ASSERT(test.cache.evictStale(timePoint) == nullptr);
  KJ_EXPECT(test.lru.currentSize() > 0);

  // Evict the entire cache.
  timePoint += 1 * kj::SECONDS;
  KJ_ASSERT(test.cache.evictStale(timePoint) == nullptr);
  KJ_EXPECT(test.lru.currentSize() == 0);
}

KJ_TEST("ActorCache purge everything while listing") {
  ActorCacheTest test({.softLimit = 1});  // evict everything immediately
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.list("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "789")]))
          .expectReturns(CAPNP(), ws);
      stream.call("values", CAPNP(list = [(key = "corge", value = "555"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) ==
        kvs({{"bar", "456"}, {"baz", "789"}, {"corge", "555"}, {"foo", "123"}}));
  }

  (void)expectUncached(test.get("bar"));
  (void)expectUncached(test.get("baz"));
  (void)expectUncached(test.get("corge"));
  (void)expectUncached(test.get("foo"));
}

KJ_TEST("ActorCache purge everything while listing; has previous entry") {
  ActorCacheTest test({.softLimit = 1});  // evict everything immediately
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // This is the same as the previous test, except we put an entry into cache first that appears
  // before the list range. This exercises a slightly different code path in markGapsEmpty().
  test.put("a", "x");

  {
    auto promise = expectUncached(test.list("bar", "qux"));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "789")]))
          .expectReturns(CAPNP(), ws);
      stream.call("values", CAPNP(list = [(key = "corge", value = "555"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) ==
        kvs({{"bar", "456"}, {"baz", "789"}, {"corge", "555"}, {"foo", "123"}}));
  }

  // Acknowledge the flush.
  mockStorage->expectCall("put", ws).thenReturn(CAPNP());
}

KJ_TEST("ActorCache exceed hard limit on read") {
  ActorCacheTest test({
    .monitorOutputGate = false,
    .softLimit = 2 * ENTRY_SIZE,
    .hardLimit = 2 * ENTRY_SIZE
  });
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto brokenPromise = test.gate.onBroken();

  {
    // Don't use expectUncached() since it will log exceptions as test failures.
    auto promise = test.list("bar"_kj, "qux"_kj).get<kj::Promise<kj::Array<KeyValue>>>();

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "789")]))
          .expectReturns(CAPNP(), ws);
      KJ_ASSERT(!brokenPromise.poll(ws));

      // The next value delivered overflows the cache.
      stream.call("values", CAPNP(list = [(key = "corge", value = "555")]))
          .expectThrows(kj::Exception::Type::OVERLOADED,
              "exceeded its memory limit due to overflowing the storage cache", ws);

      KJ_ASSERT(brokenPromise.poll(ws));

      // The exception propagates to further calls do to capnp streaming semantics.
      stream.call("values", CAPNP(list = [(key = "foo", value = "123")]))
          .expectThrows(kj::Exception::Type::OVERLOADED,
              "exceeded its memory limit due to overflowing the storage cache", ws);
      stream.call("end", CAPNP())
          .expectThrows(kj::Exception::Type::OVERLOADED,
              "exceeded its memory limit due to overflowing the storage cache", ws);

      // The call will actually have been canceled when the first call failed.
    }).expectCanceled();

    KJ_EXPECT_THROW_MESSAGE("exceeded its memory limit due to overflowing the storage cache",
        promise.wait(ws));
  }

  KJ_EXPECT_THROW_MESSAGE("exceeded its memory limit due to overflowing the storage cache",
      brokenPromise.wait(ws));
}

KJ_TEST("ActorCache exceed hard limit on write") {
  ActorCacheTest test({
    .monitorOutputGate = false,
    .softLimit = 2 * ENTRY_SIZE,
    .hardLimit = 2 * ENTRY_SIZE
  });
  auto& ws = test.ws;

  auto brokenPromise = test.gate.onBroken();

  test.put("foo", "123");
  test.put("bar", "456");
  KJ_EXPECT_THROW_MESSAGE("exceeded its memory limit due to overflowing the storage cache",
      test.put("baz", "789"));

  KJ_ASSERT(brokenPromise.poll(ws));
  KJ_EXPECT_THROW_MESSAGE("exceeded its memory limit due to overflowing the storage cache",
      brokenPromise.wait(ws));
}

// =======================================================================================

KJ_TEST("ActorCache skip cache") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Read a value.
  {
    auto promise = expectUncached(test.get("foo", {.noCache = true}));

    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "foo"))
        .thenReturn(CAPNP(value = "bar"));

    auto result = KJ_ASSERT_NONNULL(promise.wait(ws));
    KJ_EXPECT(result == "bar");
  }

  // Read it again -- not in cache!
  {
    auto promise = expectUncached(test.get("foo", {.noCache = true}));

    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "foo"))
        .thenReturn(CAPNP(value = "baz"));

    auto result = KJ_ASSERT_NONNULL(promise.wait(ws));
    KJ_EXPECT(result == "baz");
  }

  // Put a value.
  {
    test.put("foo", "qux", {.noCache = true});

    // If we read it right now, it's in cache.
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo", {.noCache = true}))) == "qux");

    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "qux")]))
        .thenReturn(CAPNP());
  }

  // Wait on the output gate to make sure the flush is actually done.
  test.gate.wait().wait(test.ws);

  // After the put completes, it's not in cache anymore.
  {
    auto promise = expectUncached(test.get("foo", {.noCache = true}));

    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "foo"))
        .thenReturn(CAPNP(value = "baz"));

    auto result = KJ_ASSERT_NONNULL(promise.wait(ws));
    KJ_EXPECT(result == "baz");
  }

  // Do it again. This time, though, the read that happens while dirty doesn't have .noCache.
  {
    test.put("foo", "qux", {.noCache = true});

    // If we read it right now, it's in cache.
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "qux");

    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "qux")]))
        .thenReturn(CAPNP());
  }

  // Wait on the output gate to make sure the flush is actually done.
  test.gate.wait().wait(test.ws);

  // This time it stayed in cache!
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "qux");

  // Do an uncached list.
  {
    auto promise = expectUncached(test.list("bar", "qux", kj::none, {.noCache = true}));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "456"),
                                          (key = "baz", value = "789"),
                                          (key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"bar", "456"}, {"baz", "789"}, {"foo", "qux"}}));
  }

  // `foo` is still cached.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "qux");

  // The other things that were returned weren't cached.
  (void)expectUncached(test.get("bar"));
  (void)expectUncached(test.get("baz"));

  // No gaps were cached as empty either.
  (void)expectUncached(test.get("corge"));
  (void)expectUncached(test.get("grault"));

  // Again, but reverse list.
  {
    auto promise = expectUncached(test.listReverse("bar", "qux", kj::none, {.noCache = true}));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123"),
                                          (key = "baz", value = "789"),
                                          (key = "bar", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"foo", "qux"}, {"baz", "789"}, {"bar", "456"}}));
  }

  // `foo` is still cached.
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "qux");

  // The other things that were returned weren't cached.
  (void)expectUncached(test.get("bar"));
  (void)expectUncached(test.get("baz"));

  // No gaps were cached as empty either.
  (void)expectUncached(test.get("corge"));
  (void)expectUncached(test.get("grault"));
}

// =======================================================================================

KJ_TEST("ActorCache transaction read-through") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  ActorCache::Transaction txn(test.cache);
  ActorCacheConvenienceWrappers eztxn(txn);

  {
    auto promise = expectUncached(eztxn.get("foo"));
    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "foo"))
        .thenReturn(CAPNP(value = "123"));
    KJ_ASSERT(KJ_ASSERT_NONNULL(promise.wait(ws)) == "123");
    KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(eztxn.get("foo"))) == "123");
  }

  {
    auto promise = expectUncached(eztxn.get("bar"));
    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "bar"))
        .thenReturn(CAPNP());
    KJ_ASSERT(promise.wait(ws) == nullptr);
    KJ_ASSERT(expectCached(eztxn.get("bar")) == nullptr);
  }

  {
    auto promise = expectUncached(eztxn.get({"baz"_kj, "qux"_kj, "corge"_kj}));

    mockStorage->expectCall("getMultiple", ws)
        .withParams(CAPNP(keys = ["baz", "corge", "qux"]), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456"),
                                          (key = "qux", value = "789")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) == kvs({{"baz", "456"}, {"qux", "789"}}));

    KJ_ASSERT(expectCached(eztxn.get({"foo"_kj, "bar"_kj, "baz"_kj, "qux"_kj, "corge"_kj})) ==
        kvs({{"baz", "456"}, {"foo", "123"}, {"qux", "789"}}));
  }

  {
    auto promise = expectUncached(eztxn.list("a", "z", 10));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "a", end = "z", limit = 10), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456"),
                                          (key = "foo", value = "123"),
                                          (key = "grault", value = "555"),
                                          (key = "qux", value = "789")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) ==
        kvs({{"baz", "456"}, {"foo", "123"}, {"grault", "555"}, {"qux", "789"}}));

    KJ_ASSERT(expectCached(eztxn.list("a", "z", 10)) ==
        kvs({{"baz", "456"}, {"foo", "123"}, {"grault", "555"}, {"qux", "789"}}));

    KJ_ASSERT(expectCached(eztxn.listReverse("a", "z")) ==
        kvs({{"qux", "789"}, {"grault", "555"}, {"foo", "123"}, {"baz", "456"}}));
  }
}

KJ_TEST("ActorCache transaction overlay changes") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  ActorCache::Transaction txn(test.cache);
  ActorCacheConvenienceWrappers eztxn(txn);

  eztxn.put("foo", "321");
  eztxn.put({{"bar", "654"}, {"qux", "987"}});
  auto deletePromise1 = expectUncached(eztxn.delete_("grault"));
  auto deletePromise2 = expectUncached(eztxn.delete_({"baz"_kj, "garply"_kj}));

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(eztxn.get("foo"))) == "321");
  KJ_ASSERT(expectCached(eztxn.get("baz")) == nullptr);
  KJ_ASSERT(expectCached(eztxn.get({"bar"_kj, "baz"_kj, "qux"_kj})) ==
      kvs({{"bar", "654"}, {"qux", "987"}}));

  // The deletes will force reads in order to compute counts.
  mockStorage->expectCall("get", ws)
      .withParams(CAPNP(key = "grault"))
      .thenReturn(CAPNP(value = "555"));
  mockStorage->expectCall("getMultiple", ws)
      .withParams(CAPNP(keys = ["baz", "garply"]), "stream"_kj)
      .useCallback("stream", [&](MockClient stream) {
    stream.call("values", CAPNP(list = [(key = "baz", value = "456")]))
        .expectReturns(CAPNP(), ws);
    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).expectCanceled();

  KJ_ASSERT(deletePromise1.wait(ws));
  KJ_ASSERT(deletePromise2.wait(ws) == 1);

  {
    auto promise = expectUncached(eztxn.get({"baz"_kj, "qux"_kj, "corge"_kj}));

    mockStorage->expectCall("getMultiple", ws)
        .withParams(CAPNP(keys = ["corge"]), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = []))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).thenReturn(CAPNP());

    KJ_ASSERT(promise.wait(ws) == kvs({{"qux", "987"}}));

    KJ_ASSERT(expectCached(eztxn.get({"foo"_kj, "bar"_kj, "baz"_kj, "qux"_kj, "corge"_kj})) ==
        kvs({{"bar", "654"}, {"foo", "321"}, {"qux", "987"}}));
  }

  {
    auto promise = expectUncached(eztxn.list("a", "z", 10));

    mockStorage->expectCall("list", ws)
        // limit is adjusted by 3 because it could return values that have already been deleted
        // in the transaction.
        .withParams(CAPNP(start = "a", end = "z", limit = 13), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "baz", value = "456"),
                                          (key = "foo", value = "123"),
                                          (key = "grault", value = "555"),
                                          (key = "qux", value = "789")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) ==
        kvs({{"bar", "654"}, {"foo", "321"}, {"qux", "987"}}));

    KJ_ASSERT(expectCached(eztxn.list("a", "z", 10)) ==
        kvs({{"bar", "654"}, {"foo", "321"}, {"qux", "987"}}));

    KJ_ASSERT(expectCached(eztxn.listReverse("a", "z")) ==
        kvs({{"qux", "987"}, {"foo", "321"}, {"bar", "654"}}));
  }

  mockStorage->expectNoActivity(ws);

  txn.commit();

  {
    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["grault", "baz"]))
        .thenReturn(CAPNP(numDeleted = 2));
    mockTxn->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "321"),
                                     (key = "bar", value = "654"),
                                     (key = "qux", value = "987")]))
        .thenReturn(CAPNP());
    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);
  }
}

KJ_TEST("ActorCache transaction overlay changes precached") {
  // Like previous test, but have the range cached in the underlying cache before the transaction
  // touches it.

  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto promise = expectUncached(test.listReverse("a", "z", 10));

    mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "a", end = "z", limit = 10, reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "qux", value = "789"),
                                          (key = "grault", value = "555"),
                                          (key = "foo", value = "123"),
                                          (key = "baz", value = "456")]))
          .expectReturns(CAPNP(), ws);
      stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
    }).expectCanceled();

    KJ_ASSERT(promise.wait(ws) ==
        kvs({{"qux", "789"}, {"grault", "555"}, {"foo", "123"}, {"baz", "456"}}));
  }

  ActorCache::Transaction txn(test.cache);
  ActorCacheConvenienceWrappers eztxn(txn);

  eztxn.put("foo", "321");
  eztxn.put({{"bar", "654"}, {"qux", "987"}});
  KJ_ASSERT(expectCached(eztxn.delete_("grault")));
  KJ_ASSERT(expectCached(eztxn.delete_({"baz"_kj, "garply"_kj})) == 1);

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectCached(eztxn.get("foo"))) == "321");
  KJ_ASSERT(expectCached(eztxn.get("baz")) == nullptr);
  KJ_ASSERT(expectCached(eztxn.get({"bar"_kj, "baz"_kj, "qux"_kj})) ==
      kvs({{"bar", "654"}, {"qux", "987"}}));

  KJ_ASSERT(expectCached(eztxn.get({"foo"_kj, "bar"_kj, "baz"_kj, "qux"_kj, "corge"_kj})) ==
      kvs({{"bar", "654"}, {"foo", "321"}, {"qux", "987"}}));
  KJ_ASSERT(expectCached(eztxn.list("a", "z", 10)) ==
      kvs({{"bar", "654"}, {"foo", "321"}, {"qux", "987"}}));
  KJ_ASSERT(expectCached(eztxn.listReverse("a", "z")) ==
      kvs({{"qux", "987"}, {"foo", "321"}, {"bar", "654"}}));

  mockStorage->expectNoActivity(ws);

  txn.commit();

  {
    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("delete", ws)
        .withParams(CAPNP(keys = ["grault", "baz"]))
        .thenReturn(CAPNP(numDeleted = 2));
    mockTxn->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "321"),
                                     (key = "bar", value = "654"),
                                     (key = "qux", value = "987")]))
        .thenReturn(CAPNP());
    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);
  }
}

KJ_TEST("ActorCache transaction output gate blocked during flush") {
  ActorCacheTest test({.monitorOutputGate = false});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Gate is currently not blocked.
  test.gate.wait().wait(ws);

  // Do a transaction with a put.
  ActorCache::Transaction txn(test.cache);
  ActorCacheConvenienceWrappers eztxn(txn);
  eztxn.put("foo", "123");
  txn.commit();

  // Now it is blocked.
  auto gatePromise = test.gate.wait();
  KJ_ASSERT(!gatePromise.poll(ws));

  // Complete the transaction.
  {
    auto inProgressFlush = mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "123")]));

    // Still blocked until the flush completes.
    KJ_ASSERT(!gatePromise.poll(ws));

    kj::mv(inProgressFlush).thenReturn(CAPNP());
  }

  KJ_ASSERT(gatePromise.poll(ws));
  gatePromise.wait(ws);
}

KJ_TEST("ActorCache transaction output gate bypass") {
  ActorCacheTest test({.monitorOutputGate = false});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Gate is currently not blocked.
  test.gate.wait().wait(ws);

  // Do a transaction with a put.
  ActorCache::Transaction txn(test.cache);
  ActorCacheConvenienceWrappers eztxn(txn);
  eztxn.put("foo", "123", {.allowUnconfirmed = true});
  txn.commit();

  // Gate still isn't blocked, because we set `allowUnconfirmed`.
  test.gate.wait().wait(ws);

  // Complete the transaction with a flush.
  mockStorage->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "foo", value = "123")]))
      .thenReturn(CAPNP());

  test.gate.wait().wait(ws);
}

KJ_TEST("ActorCache transaction output gate bypass on one put but not the next") {
  ActorCacheTest test({.monitorOutputGate = false});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Gate is currently not blocked.
  test.gate.wait().wait(ws);

  // Do a transaction with two puts, only bypassing on the first. The net result should be that
  // the output gate is in effect.
  ActorCache::Transaction txn(test.cache);
  ActorCacheConvenienceWrappers eztxn(txn);
  eztxn.put("foo", "123", {.allowUnconfirmed = true});
  eztxn.put("bar", "456");
  txn.commit();

  // Now it is blocked.
  auto gatePromise = test.gate.wait();
  KJ_ASSERT(!gatePromise.poll(ws));

  // Complete the transaction.
  {
    auto inProgressFlush = mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "123"), (key = "bar", value = "456")]));

    // Still blocked until the flush completes.
    KJ_ASSERT(!gatePromise.poll(ws));

    kj::mv(inProgressFlush).thenReturn(CAPNP());
  }

  KJ_ASSERT(gatePromise.poll(ws));
  gatePromise.wait(ws);
}

KJ_TEST("ActorCache transaction multiple put batches") {
  ActorCacheTest test({.maxKeysPerRpc = 2});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Do a transaction with enough puts to batch.
  ActorCache::Transaction txn(test.cache);
  ActorCacheConvenienceWrappers eztxn(txn);
  eztxn.put({{"foo", "123"}, {"bar", "456"}, {"baz", "789"}});

  // Poll the wait scope to make sure we haven't slipped through to the cache already.
  ws.poll();

  eztxn.put({{"qux", "555"}, {"corge", "999"}});
  txn.commit();

  auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
  mockTxn->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "foo", value = "123"),
                                    (key = "bar", value = "456")]))
      .thenReturn(CAPNP());
  mockTxn->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "baz", value = "789"),
                                    (key = "qux", value = "555")]))
      .thenReturn(CAPNP());
  mockTxn->expectCall("put", ws)
      .withParams(CAPNP(entries = [(key = "corge", value = "999")]))
      .thenReturn(CAPNP());
  mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
  mockTxn->expectDropped(ws);
}


KJ_TEST("ActorCache transaction multiple counted delete batches") {
  // Do a transaction with a big counted delete. The rpc getMultiple and delete should batch
  // according to maxKeysPerRpc.

  ActorCacheTest test({.maxKeysPerRpc = 2});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  ActorCache::Transaction txn(test.cache);
  ActorCacheConvenienceWrappers eztxn(txn);

  {
    // Load one of our values to delete into the cache itself which will avoid rpc deletes for
    // counting.
    test.put("count2", "2");
    mockStorage->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "count2", value = "2")]))
        .thenReturn(CAPNP());
  }

  {
    // Load one of our values to delete into the transaction which will avoid even talking to the
    // cache.
    eztxn.put("count3", "3");
  }

  auto deletePromise = eztxn.delete_(
      {"count1"_kj, "count2"_kj, "count3"_kj, "count4"_kj, "count5"_kj}).get<kj::Promise<uint>>();

  mockStorage->expectCall("getMultiple", ws)
      // Note that this batch is smaller because "count2" was known to the actor cache.
      .withParams(CAPNP(keys = ["count1"]), "stream"_kj)
      .useCallback("stream", [&](MockClient stream) {
    // Pretend that "count1" already exists but was not in the cache.
    stream.call("values", CAPNP(list = [(key = "count1", value = "1")]))
        .expectReturns(CAPNP(), ws);
    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).thenReturn(CAPNP());
  mockStorage->expectCall("getMultiple", ws)
      .withParams(CAPNP(keys = ["count4", "count5"]), "stream"_kj)
      .useCallback("stream", [&](MockClient stream) {
    stream.call("end", CAPNP()).expectReturns(CAPNP(), ws);
  }).thenReturn(CAPNP());

  // For hacky reasons, we are able to observe the counted delete before we submit the
  // transaction.
  KJ_EXPECT(deletePromise.wait(ws) == 3);

  txn.commit();

  auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
  mockTxn->expectCall("delete", ws)
      // "count3" comes first because it entered the transaction first.
      .withParams(CAPNP(keys = ["count3", "count1"]))
      .thenReturn(CAPNP(numDeleted = 1));
  mockTxn->expectCall("delete", ws)
      // Neither "count4" or "count5" are deleted because we observed them in the get.
      .withParams(CAPNP(keys = ["count2"]))
      .thenReturn(CAPNP(numDeleted = 1));
  mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
  mockTxn->expectDropped(ws);
}

KJ_TEST("ActorCache transaction negative list range returns nothing") {
  ActorCacheTest test({.monitorOutputGate = false});

  ActorCache::Transaction txn(test.cache);
  ActorCacheConvenienceWrappers eztxn(txn);

  eztxn.put("foo", "123");

  KJ_ASSERT(expectCached(eztxn.list("qux", "bar")) == kvs({}));
  KJ_ASSERT(expectCached(eztxn.listReverse("qux", "bar")) == kvs({}));
}

// =======================================================================================

KJ_TEST("ActorCache list stream cancellation") {
  // Test for cases where implementations of ListStream might stay alive longer than expected, due
  // to capabilities being held remotely.
  //
  // We can't use ActorCacheTest in this test as we need to manage allocation and destruction to
  // set up the problematic circumstances.

  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto mockPair = MockServer::make<rpc::ActorStorage::Stage>();
  kj::Own<MockServer> mockStorage = kj::mv(mockPair.mock);
  auto mockClient = kj::mv(mockPair.client);

  ActorCacheTestOptions options;

  kj::Maybe<MockServer::ExpectedCall> call;
  kj::Maybe<MockClient> listClient;

  // Try get-multiple.
  {
    // We allocate `lru` on the heap to assist valgrind in being able to detect when it is used
    // after free.
    auto lru = kj::heap<ActorCache::SharedLru>(ActorCache::SharedLru::Options {
        options.softLimit, options.hardLimit, options.staleTimeout, options.dirtyListByteLimit,
        options.maxKeysPerRpc});
    OutputGate gate;
    ActorCache cache(mockClient, *lru, gate);

    ActorCacheConvenienceWrappers ezCache(cache);

    auto promise = expectUncached(ezCache.get({"foo"_kj, "bar"_kj}));

    call = mockStorage->expectCall("getMultiple", ws)
        .withParams(CAPNP(keys = ["bar", "foo"]), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "123")]))
          .expectReturns(CAPNP(), ws);
      listClient = kj::mv(stream);
    });

    // Now we're going to cancel the promise and destroy the cache while the call is still
    // outstanding, with unreported entries in it!
  }

  KJ_ASSERT_NONNULL(listClient).call("values", CAPNP(list = [(key = "foo", value = "456")]))
      .expectThrows(kj::Exception::Type::DISCONNECTED, "canceled", ws);
  KJ_ASSERT_NONNULL(kj::mv(call)).expectCanceled();

  // Try list().
  {
    // We allocate `lru` on the heap to assist valgrind in being able to detect when it is used
    // after free.
    auto lru = kj::heap<ActorCache::SharedLru>(ActorCache::SharedLru::Options {
        options.softLimit, options.hardLimit, options.staleTimeout, options.dirtyListByteLimit,
        options.maxKeysPerRpc});
    OutputGate gate;
    ActorCache cache(mockClient, *lru, gate);

    ActorCacheConvenienceWrappers ezCache(cache);

    auto promise = expectUncached(ezCache.list("bar", "qux"));

    call = mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux"), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "bar", value = "123")]))
          .expectReturns(CAPNP(), ws);
      listClient = kj::mv(stream);
    });

    // Now we're going to cancel the promise and destroy the cache while the call is still
    // outstanding, with unreported entries in it!
  }

  KJ_ASSERT_NONNULL(listClient).call("values", CAPNP(list = [(key = "foo", value = "456")]))
      .expectThrows(kj::Exception::Type::DISCONNECTED, "canceled", ws);
  KJ_ASSERT_NONNULL(kj::mv(call)).expectCanceled();

  // Try listReverse().
  {
    // We allocate `lru` on the heap to assist valgrind in being able to detect when it is used
    // after free.
    auto lru = kj::heap<ActorCache::SharedLru>(ActorCache::SharedLru::Options {
        options.softLimit, options.hardLimit, options.staleTimeout, options.dirtyListByteLimit,
        options.maxKeysPerRpc});
    OutputGate gate;
    ActorCache cache(mockClient, *lru, gate);

    ActorCacheConvenienceWrappers ezCache(cache);

    auto promise = expectUncached(ezCache.listReverse("bar", "qux"));

    call = mockStorage->expectCall("list", ws)
        .withParams(CAPNP(start = "bar", end = "qux", reverse = true), "stream"_kj)
        .useCallback("stream", [&](MockClient stream) {
      stream.call("values", CAPNP(list = [(key = "foo", value = "123")]))
          .expectReturns(CAPNP(), ws);
      listClient = kj::mv(stream);
    });

    // Now we're going to cancel the promise and destroy the cache while the call is still
    // outstanding, with unreported entries in it!
  }

  KJ_ASSERT_NONNULL(listClient).call("values", CAPNP(list = [(key = "bar", value = "456")]))
      .expectThrows(kj::Exception::Type::DISCONNECTED, "canceled", ws);
  KJ_ASSERT_NONNULL(kj::mv(call)).expectCanceled();
}

KJ_TEST("ActorCache never-flush") {
  ActorCacheTest test({.neverFlush = true});
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  // Puts don't start a transaction.
  KJ_EXPECT(test.put("foo", "123") == nullptr);
  KJ_EXPECT(test.cache.onNoPendingFlush() == nullptr);
  mockStorage->expectNoActivity(ws);

  // Gets still see the put() value.
  KJ_EXPECT(KJ_ASSERT_NONNULL(expectCached(test.get("foo"))) == "123");

  // Uncached reads work normally.
  {
    auto promise = expectUncached(test.get("bar"));

    mockStorage->expectCall("get", ws)
        .withParams(CAPNP(key = "bar"))
        .thenReturn(CAPNP(value = "456"));

    auto result = KJ_ASSERT_NONNULL(promise.wait(ws));
    KJ_EXPECT(result == "456");
  }
}

KJ_TEST("ActorCache alarm get/put") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  {
    auto time = expectUncached(test.getAlarm());

    mockStorage->expectCall("getAlarm", ws)
      .thenReturn(CAPNP(scheduledTimeMs = 0));

    KJ_ASSERT(time.wait(ws) == kj::none);
  }

  {
    auto time = expectCached(test.getAlarm());

    KJ_ASSERT(time == kj::none);
  }

  auto oneMs = 1 * kj::MILLISECONDS + kj::UNIX_EPOCH;
  auto twoMs = 2 * kj::MILLISECONDS + kj::UNIX_EPOCH;
  {
    // Test alarm writes happen transactionally with storage ops
    test.setAlarm(oneMs);
    test.put("foo", "bar");

    auto mockTxn = mockStorage->expectCall("txn", ws).returnMock("transaction");
    mockTxn->expectCall("put", ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "bar")]))
        .thenReturn(CAPNP());
    mockTxn->expectCall("setAlarm", ws)
        .withParams(CAPNP(scheduledTimeMs = 1))
        .thenReturn(CAPNP());
    mockTxn->expectCall("commit", ws).thenReturn(CAPNP());
    mockTxn->expectDropped(ws);
  }

  {
    auto time = expectCached(test.getAlarm());

    KJ_ASSERT(time == oneMs);
  }

  {
    // Test clearing alarm
    test.setAlarm(kj::none);

    // When there are no other storage operations to be flushed, alarm modifications can be flushed
    // without a wrapping txn.
    mockStorage->expectCall("deleteAlarm", ws)
        .withParams(CAPNP(timeToDeleteMs = 0))
        .thenReturn(CAPNP(deleted = true));
    // Wait on the output gate to make sure the flush is actually done before checking the cache.
    test.gate.wait().wait(test.ws);
  }

  {
    auto time = expectCached(test.getAlarm());

    KJ_ASSERT(time == kj::none);
  }

  // we have a cached time == nullptr, so we should not attempt to run an alarm
  KJ_ASSERT(test.cache.armAlarmHandler(10 * kj::SECONDS + kj::UNIX_EPOCH, false) == kj::none);

  {
    test.setAlarm(oneMs);

    mockStorage->expectCall("setAlarm", ws)
        .withParams(CAPNP(scheduledTimeMs = 1))
        .thenReturn(CAPNP());
  }

  {
    // Test that alarm handler handle clears alarm when dropped with no writes
    {
      auto maybeWrite = KJ_ASSERT_NONNULL(test.cache.armAlarmHandler(oneMs, false));
    }
    mockStorage->expectCall("deleteAlarm", ws)
        .withParams(CAPNP(timeToDeleteMs = 1))
        .thenReturn(CAPNP(deleted = true));
  }

  {
    test.setAlarm(oneMs);

    // Test that alarm handler handle does not clear alarm when dropped with writes
    {
      auto maybeWrite = KJ_ASSERT_NONNULL(test.cache.armAlarmHandler(oneMs, false));
      test.setAlarm(twoMs);
    }
    mockStorage->expectCall("setAlarm", ws)
        .withParams(CAPNP(scheduledTimeMs = 2))
        .thenReturn(CAPNP());
  }

  {
    test.setAlarm(oneMs);

    // Test that alarm handler handle does not cache delete when it fails
    {
      auto maybeWrite = KJ_ASSERT_NONNULL(test.cache.armAlarmHandler(oneMs, false));
    }
    mockStorage->expectCall("deleteAlarm", ws)
        .withParams(CAPNP(timeToDeleteMs = 1))
        .thenReturn(CAPNP(deleted = false));
    test.gate.wait().wait(test.ws);
  }

  {
    // Test that alarm handler handle does not cache alarm delete when noCache == true
    {
      auto maybeWrite = KJ_ASSERT_NONNULL(test.cache.armAlarmHandler(twoMs, true));
    }
    mockStorage->expectCall("deleteAlarm", ws)
        .withParams(CAPNP(timeToDeleteMs = 2))
        .thenReturn(CAPNP(deleted = true));
    test.gate.wait().wait(test.ws);
  }

  {
    auto time = expectUncached(test.getAlarm());

    mockStorage->expectCall("getAlarm", ws)
      .thenReturn(CAPNP(scheduledTimeMs = 0));

    KJ_ASSERT(time.wait(ws) == kj::none);
  }
}

KJ_TEST("ActorCache uncached nonnull alarm get") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto time = expectUncached(test.getAlarm());
  auto oneMs = 1 * kj::MILLISECONDS + kj::UNIX_EPOCH;

  mockStorage->expectCall("getAlarm", ws)
    .thenReturn(CAPNP(scheduledTimeMs = 1));

  KJ_ASSERT(time.wait(ws) == oneMs);
}

KJ_TEST("ActorCache alarm delete when flush fails") {
  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  auto oneMs = 1 * kj::MILLISECONDS + kj::UNIX_EPOCH;

  {
    auto time = expectUncached(test.getAlarm());

    mockStorage->expectCall("getAlarm", ws)
      .thenReturn(CAPNP(scheduledTimeMs = 1));

    KJ_ASSERT(time.wait(ws) == oneMs);
  }

  {
    auto time = KJ_ASSERT_NONNULL(expectCached(test.getAlarm()));
    KJ_ASSERT(time == oneMs);
  }

  // we want to test that even if a flush is retried
  // that the post-delete actions for a checked delete happen.
  {
    auto handle = test.cache.armAlarmHandler(oneMs, false);

    auto time = expectCached(test.getAlarm());
    KJ_ASSERT(time == kj::none);
  }

  for(auto i = 0; i < 2; i++) {
    mockStorage->expectCall("deleteAlarm", ws)
        .withParams(CAPNP(timeToDeleteMs = 1))
        .thenThrow(KJ_EXCEPTION(DISCONNECTED, "foo"));
  }

  {
    mockStorage->expectCall("deleteAlarm", ws)
        .withParams(CAPNP(timeToDeleteMs = 1))
        .thenReturn(CAPNP(deleted = false));
    // Wait on the output gate to make sure the flush is actually done.
    test.gate.wait().wait(test.ws);
  }

  {
    auto time = expectUncached(test.getAlarm());

    mockStorage->expectCall("getAlarm", ws)
      .thenReturn(CAPNP(scheduledTimeMs = 10));

    KJ_ASSERT(time.wait(ws) == 10 * kj::MILLISECONDS + kj::UNIX_EPOCH);
  }
}

KJ_TEST("ActorCache can wait for flush") {
  // This test confirms that `onNoPendingFlush()` will return a promise that resolves when any
  // scheduled or in-flight flush completes.

  ActorCacheTest test;
  auto& ws = test.ws;
  auto& mockStorage = test.mockStorage;

  struct InFlightRequest {
    workerd::MockServer::ExpectedCall op;
    kj::Maybe<kj::Own<workerd::MockServer>> maybeTxn;
  };

  // There is no pending flush since nothing has been done!
  KJ_ASSERT(test.cache.onNoPendingFlush() == nullptr);

  struct VerifyOptions {
    bool skipSecondOperation;
  };
  size_t secondaryPutIndex = 0;
  auto verify = [&](auto receiveRequest, auto sendResponse, VerifyOptions options) {
    // We haven't sent our request yet, but we should have a promise now.
    auto scheduledPromise = KJ_ASSERT_NONNULL(test.cache.onNoPendingFlush());

    // We have sent our request, but it hasn't responded yet. We should still have a promise.
    auto req = receiveRequest();
    auto inFlightPromise = KJ_ASSERT_NONNULL(test.cache.onNoPendingFlush());

    // Do an additional put to make a separate flush.
    struct SecondOperation {
      kj::String key;
      kj::Promise<void> scheduledPromise;
    };
    kj::Maybe<SecondOperation> maybeSecondOperation;
    if (!options.skipSecondOperation) {
      auto key = kj::str("foo-", secondaryPutIndex++);
      test.put(key, "bar");
      auto secondPromise = KJ_ASSERT_NONNULL(test.cache.onNoPendingFlush());
      KJ_ASSERT(!secondPromise.poll(ws));
      maybeSecondOperation.emplace(SecondOperation{
        .key = kj::mv(key),
        .scheduledPromise = kj::mv(secondPromise),
      });
    }

    // No promise should have resolved yet.
    KJ_ASSERT(!scheduledPromise.poll(ws) && !inFlightPromise.poll(ws));

    // Resolve the operations and confirm that the promises resolve.
    sendResponse(kj::mv(req));
    scheduledPromise.wait(ws);
    inFlightPromise.wait(ws);

    KJ_IF_SOME(secondOperation, maybeSecondOperation) {
      // This promise is for a later flush, so it should not have resolved yet.
      KJ_ASSERT(!secondOperation.scheduledPromise.poll(ws));

      // Finish our secondary put and observe the second flush resolving.
      auto params =
          kj::str(R"((entries = [(key = ")", secondOperation.key, R"(", value = "bar")]))");
      mockStorage->expectCall("put", ws).withParams(params).thenReturn(CAPNP());

      secondOperation.scheduledPromise.wait(ws);
    }

    // We finished our flush, nothing left to do.
    KJ_ASSERT(test.cache.onNoPendingFlush() == nullptr);
  };

  {
    // Join in on a simple put.
    test.put("foo", "bar");

    verify([&](){
      return InFlightRequest {
        .op = mockStorage->expectCall("put", ws)
          .withParams(CAPNP(entries = [(key = "foo", value = "bar")])),
      };
    }, [&](auto req) {
      kj::mv(req.op).thenReturn(CAPNP());
    }, {
      .skipSecondOperation = false,
    });
  }

  {
    // Join in on a delete.
    test.delete_("foo");

    verify([&](){
      return InFlightRequest {
        .op = mockStorage->expectCall("delete", ws).withParams(CAPNP(keys = ["foo"])),
      };
    }, [&](auto req) {
      kj::mv(req.op).thenReturn(CAPNP(numDeleted = 1));
    }, {
      .skipSecondOperation = false,
    });
  }

  {
    // Join in on a simple put with allowUnconfirmed.
    test.put("foo", "baz", ActorCacheWriteOptions{.allowUnconfirmed = true});

    verify([&](){
      return InFlightRequest {
        .op = mockStorage->expectCall("put", ws)
          .withParams(CAPNP(entries = [(key = "foo", value = "baz")])),
      };
    }, [&](auto req) {
      kj::mv(req.op).thenReturn(CAPNP());
    }, {
      .skipSecondOperation = false,
    });
  }

  {
    // Join in on a delete with allowUnconfirmed.
    test.delete_("foo", ActorCacheWriteOptions{.allowUnconfirmed = true});

    verify([&](){
      return InFlightRequest {
        .op = mockStorage->expectCall("delete", ws).withParams(CAPNP(keys = ["foo"])),
      };
    }, [&](auto req) {
      kj::mv(req.op).thenReturn(CAPNP(numDeleted = 1));
    }, {
      .skipSecondOperation = false,
    });
  }

  {
    // Join in on a scheduled setAlarm.
    test.setAlarm(1 * kj::MILLISECONDS + kj::UNIX_EPOCH);

    verify([&](){
      return InFlightRequest {
        .op = mockStorage->expectCall("setAlarm", ws).withParams(CAPNP(scheduledTimeMs = 1)),
      };
    }, [&](auto req) {
      kj::mv(req.op).thenReturn(CAPNP());
    }, {
      .skipSecondOperation = false,
    });
  }

  {
    // Join in on a scheduled setAlarm with allowUnconfirmed.
    test.setAlarm(
        2 * kj::MILLISECONDS + kj::UNIX_EPOCH, ActorCacheWriteOptions{.allowUnconfirmed = true});

    verify([&](){
      return InFlightRequest {
        .op = mockStorage->expectCall("setAlarm", ws).withParams(CAPNP(scheduledTimeMs = 2)),
      };
    }, [&](auto req) {
      kj::mv(req.op).thenReturn(CAPNP());
    }, {
      .skipSecondOperation = false,
    });
  }

  {
    // Join in on a scheduled deleteAll.
    test.cache.deleteAll(ActorCacheWriteOptions{.allowUnconfirmed = false});

    verify([&](){
      return InFlightRequest {
        .op = mockStorage->expectCall("deleteAll", ws).withParams(CAPNP()),
      };
    }, [&](auto req) {
      kj::mv(req.op).thenReturn(CAPNP());
    }, {
      // We can't test the second operation because deleteAll immediately follows up with any puts
      // that happened while it was in flight. This means that we invoke the mock twice in the same
      // promise chain without being able to set up expections in time.
      .skipSecondOperation = true,
    });
  }

  {
    // Join in on a scheduled deleteAll with allowUnconfirmed.
    test.cache.deleteAll(ActorCacheWriteOptions{.allowUnconfirmed = true});

    verify([&](){
      return InFlightRequest {
        .op = mockStorage->expectCall("deleteAll", ws).withParams(CAPNP()),
      };
    }, [&](auto req) {
      kj::mv(req.op).thenReturn(CAPNP());
    }, {
      .skipSecondOperation = true,
    });
  }
}


KJ_TEST("ActorCache can shutdown") {
  // This test confirms that `shutdown()` stops scheduled flushes but does not stop in-flight
  // flushes. It also confirms that `shutdown()` prevents future operations.

  struct InFlightRequest {
    MockServer::ExpectedCall op;
    kj::Promise<void> promise;
  };

  struct BeforeShutdownResult {
    kj::Maybe<InFlightRequest> maybeReq;
    bool shouldBreakOutputGate;
  };

  struct VerifyOptions {
    kj::Maybe<const kj::Exception&> maybeError;
  };
  auto verifyWithOptions = [&](auto&& beforeShutdown, auto&& afterShutdown, VerifyOptions options) {
    auto test = ActorCacheTest({.monitorOutputGate = false});
    auto& ws = test.ws;

    BeforeShutdownResult res = beforeShutdown(test);

    // Shutdown and observe the pending flush to break the io gate.
    test.cache.shutdown(options.maybeError);
    auto maybeShutdownPromise = test.cache.onNoPendingFlush();

    afterShutdown(test, kj::mv(res.maybeReq));

    auto errorMessage = options.maybeError.map([](const kj::Exception& e){
      return e.getDescription();
    }).orDefault(ActorCache::SHUTDOWN_ERROR_MESSAGE);

    if (res.shouldBreakOutputGate) {
      // We expected the output gate to break async after shutdown.
      auto& shutdownPromise = KJ_REQUIRE_NONNULL(maybeShutdownPromise);
      KJ_EXPECT_THROW_MESSAGE(errorMessage, shutdownPromise.wait(ws));
      KJ_EXPECT(test.cache.onNoPendingFlush() == nullptr);
      KJ_EXPECT_THROW_MESSAGE(errorMessage, test.gate.wait().wait(ws));
    } else KJ_IF_SOME(promise, maybeShutdownPromise) {
      // The in-flight flush should resolve cleanly without any follow on or breaking the output
      // gate.
      promise.wait(ws);
      KJ_EXPECT(test.cache.onNoPendingFlush() == nullptr);
      test.gate.wait().wait(ws);
    }

    // Puts and deletes, even with allowedUnconfirmed, should throw.
    KJ_EXPECT_THROW_MESSAGE(errorMessage, test.put("foo", "baz"));
    KJ_EXPECT_THROW_MESSAGE(errorMessage, test.put("foo", "bat", {.allowUnconfirmed = true}));
    KJ_EXPECT_THROW_MESSAGE(errorMessage, test.delete_("foo"));
    KJ_EXPECT_THROW_MESSAGE(errorMessage, test.delete_("foo", {.allowUnconfirmed = true}));

    if (!res.shouldBreakOutputGate) {
      // We tried to use storage after shutdown, we should now be breaking the output gate.
      auto afterShutdownPromise = KJ_ASSERT_NONNULL(test.cache.onNoPendingFlush());
      KJ_EXPECT_THROW_MESSAGE(errorMessage, afterShutdownPromise.wait(ws));
      KJ_EXPECT(test.cache.onNoPendingFlush() == nullptr);
      KJ_EXPECT_THROW_MESSAGE(errorMessage, test.gate.wait().wait(ws));
    }
  };

  auto verify = [&](auto&& beforeShutdown, auto&& afterShutdown) {
    verifyWithOptions(beforeShutdown, afterShutdown, {.maybeError = kj::none});
    verifyWithOptions(beforeShutdown, afterShutdown, {.maybeError = KJ_EXCEPTION(FAILED, "Nope.")});
  };

  verify([](ActorCacheTest& test){
    // Do nothing and expect nothing!
    return BeforeShutdownResult{
      .maybeReq = kj::none,
      .shouldBreakOutputGate = false,
    };
  }, [](ActorCacheTest& test, kj::Maybe<InFlightRequest>){
    // Nothing should have made it to storage.
    test.mockStorage->expectNoActivity(test.ws);
  });

  verify([](ActorCacheTest& test){
    // Do a confirmed put (which schedules a flush).
    test.put("foo", "bar", {.allowUnconfirmed = false});

    // Expect the put to be cancelled and break the gate.
    return BeforeShutdownResult{
      .maybeReq = kj::none,
      .shouldBreakOutputGate = true,
    };
  }, [](ActorCacheTest& test, kj::Maybe<InFlightRequest>){
    // Nothing should have made it to storage.
    test.mockStorage->expectNoActivity(test.ws);
  });

  verify([](ActorCacheTest& test){
    // Do an unconfirmed put (which schedules a flush).
    test.put("foo", "bar", {.allowUnconfirmed = true});

    // Expect the put to be cancelled and break the gate.
    return BeforeShutdownResult{
      .maybeReq = kj::none,
      .shouldBreakOutputGate = true,
    };
  }, [](ActorCacheTest& test, kj::Maybe<InFlightRequest>){
    // Nothing should have made it to storage.
    test.mockStorage->expectNoActivity(test.ws);
  });

  verify([](ActorCacheTest& test) {
    // Do a confirmed put and wait for it to be in-flight.
    test.put("foo", "bar", {.allowUnconfirmed = false});

    auto op = test.mockStorage->expectCall("put", test.ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "bar")]));
    auto promise = KJ_REQUIRE_NONNULL(test.cache.onNoPendingFlush());
    KJ_EXPECT(!promise.poll(test.ws));

    return BeforeShutdownResult{
      .maybeReq = InFlightRequest{
        .op = kj::mv(op),
        .promise = kj::mv(promise),
      },
      .shouldBreakOutputGate = false,
    };
  }, [](ActorCacheTest& test, kj::Maybe<InFlightRequest> maybeReq){
    // Finish the storage response and wait to see our pre-shutdown in-flight flush finish.
    auto req = KJ_ASSERT_NONNULL(kj::mv(maybeReq));
    kj::mv(req.op).thenReturn(CAPNP());
    req.promise.wait(test.ws);

    // Nothing else should have made it to storage.
    test.mockStorage->expectNoActivity(test.ws);
  });


  verify([](ActorCacheTest& test) {
    // Do an unconfirmed put and wait for it to be in-flight.
    test.put("foo", "bar", {.allowUnconfirmed = true});

    auto op = test.mockStorage->expectCall("put", test.ws)
        .withParams(CAPNP(entries = [(key = "foo", value = "bar")]));
    auto promise = KJ_REQUIRE_NONNULL(test.cache.onNoPendingFlush());
    KJ_EXPECT(!promise.poll(test.ws));

    return BeforeShutdownResult{
      .maybeReq = InFlightRequest{
        .op = kj::mv(op),
        .promise = kj::mv(promise),
      },
      .shouldBreakOutputGate = false,
    };
  }, [](ActorCacheTest& test, kj::Maybe<InFlightRequest> maybeReq){
    // Finish the storage response and wait to see our pre-shutdown in-flight flush finish.
    auto req = KJ_ASSERT_NONNULL(kj::mv(maybeReq));
    kj::mv(req.op).thenReturn(CAPNP());
    req.promise.wait(test.ws);

    // Nothing else should have made it to storage.
    test.mockStorage->expectNoActivity(test.ws);
  });
}

}  // namespace
}  // namespace workerd
