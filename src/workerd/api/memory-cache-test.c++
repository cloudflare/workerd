// Tests for the C++ side of the memory cache: binding lifetime, singleflight
// leader/waiter handoff, the FallbackDoneCallback contract, and the trace spans of
// MemoryCache.read(), which the Rust MemoryCache must record as the C++ one does. The
// storage and coordination logic itself is covered by the Rust crate's own tests.

#include "memory-cache.h"

#include <workerd/api/memory-cache-api/ffi.rs.h>
#include <workerd/io/trace.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/thread-scopes.h>

#include <kj/test.h>
#include <kj/thread.h>

#include <algorithm>

namespace workerd::api {
namespace {

using Outcome = MemoryCacheUse::GetWithFallbackOutcome;
using FallbackDoneCallback = MemoryCacheUse::FallbackDoneCallback;

static MemoryCacheLimits testLimits() {
  return {
    .maxKeys = 100,
    .maxValueSize = 1024,
    .maxTotalValueSize = 10240,
  };
}

KJ_TEST("serializes concurrent final release and acquisition") {
  MemoryCacheProvider provider;
  auto run = [&provider]() {
    for (size_t i = 0; i < 1000; ++i) {
      auto binding = provider.getUse("shared"_kj, testLimits());
    }
  };
  {
    kj::Thread first(run);
    kj::Thread second(run);
  }
  auto first = provider.getUse("shared"_kj, testLimits());
  auto second = provider.getUse("shared"_kj, testLimits());
  KJ_EXPECT(first->getStatsForTest().bindings == 2);
}

KJ_TEST("provider teardown does not invalidate a live binding") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    kj::Own<MemoryCacheUse> use;
    kj::Own<MemoryCacheUse> privateUse;
    {
      MemoryCacheProvider provider;
      use = provider.getUse("shared"_kj, testLimits());
      privateUse = provider.getUse(kj::none, testLimits());
    }

    SpanBuilder span(nullptr);
    auto result = use->getWithFallback(kj::str("key"), span);
    KJ_ASSERT(result.is<kj::Promise<Outcome>>());
    return kj::mv(result.get<kj::Promise<Outcome>>())
        .then([use = kj::mv(use), privateUse = kj::mv(privateUse)](Outcome outcome) mutable {
      (void)use;
      (void)privateUse;
      KJ_ASSERT(outcome.is<FallbackDoneCallback>());
      SpanBuilder span(nullptr);
      outcome.get<FallbackDoneCallback>()(kj::none, span);
    });
  });
}

KJ_TEST("canceled waiters unlink immediately") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    MemoryCacheProvider provider;
    auto cache = provider.getUse("shared"_kj, testLimits());
    auto key = kj::str("key");
    SpanBuilder span(nullptr);
    auto leader = cache->getWithFallback(key, span);
    KJ_ASSERT(leader.is<kj::Promise<Outcome>>());
    {
      auto follower = cache->getWithFallback(key, span);
      KJ_ASSERT(follower.is<kj::Promise<Outcome>>());
      KJ_EXPECT(cache->getStatsForTest().waiters == 1);
    }
    KJ_EXPECT(cache->getStatsForTest().waiters == 0);
    KJ_EXPECT(cache->getStatsForTest().canceledWaiters == 1);

    return kj::mv(leader.get<kj::Promise<Outcome>>())
        .then([cache = kj::mv(cache)](Outcome outcome) mutable {
      KJ_ASSERT(outcome.is<FallbackDoneCallback>());
      SpanBuilder span(nullptr);
      outcome.get<FallbackDoneCallback>()(kj::none, span);
      auto stats = cache->getStatsForTest();
      KJ_EXPECT(stats.inFlightFallbacks == 0);
      KJ_EXPECT(stats.waiters == 0);
      KJ_EXPECT(stats.canceledWaiters == 1);
    });
  });
}

KJ_TEST("abandoned fallback token promotes the next waiter") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    MemoryCacheProvider provider;
    auto cache = provider.getUse("shared"_kj, testLimits());
    auto key = kj::str("key");
    SpanBuilder span(nullptr);
    auto leader = cache->getWithFallback(key, span);
    auto follower = cache->getWithFallback(key, span);
    KJ_ASSERT(leader.is<kj::Promise<Outcome>>());
    KJ_ASSERT(follower.is<kj::Promise<Outcome>>());
    auto followerPromise = kj::mv(follower.get<kj::Promise<Outcome>>());
    return kj::mv(leader.get<kj::Promise<Outcome>>())
        .then([follower = kj::mv(followerPromise), cache = kj::mv(cache)](Outcome outcome) mutable {
      KJ_ASSERT(outcome.is<FallbackDoneCallback>());
      { auto abandoned = kj::mv(outcome.get<FallbackDoneCallback>()); }
      return kj::mv(follower).then([cache = kj::mv(cache)](Outcome outcome) {
        KJ_ASSERT(outcome.is<FallbackDoneCallback>());
        SpanBuilder span(nullptr);
        outcome.get<FallbackDoneCallback>()(kj::none, span);
        KJ_EXPECT(cache->getStatsForTest().inFlightFallbacks == 0);
      });
    });
  });
}

KJ_TEST("fallback callback is one-shot") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    MemoryCacheProvider provider;
    auto cache = provider.getUse("shared"_kj, testLimits());
    auto key = kj::str("key");
    SpanBuilder span(nullptr);
    auto leader = cache->getWithFallback(key, span);
    KJ_ASSERT(leader.is<kj::Promise<Outcome>>());
    return kj::mv(leader.get<kj::Promise<Outcome>>()).then([](Outcome outcome) {
      KJ_ASSERT(outcome.is<FallbackDoneCallback>());
      auto callback = kj::mv(outcome.get<FallbackDoneCallback>());
      SpanBuilder span(nullptr);
      callback(kj::none, span);
      KJ_EXPECT_THROW_MESSAGE(
          "memory cache fallback callback invoked more than once", callback(kj::none, span));
    });
  });
}

KJ_TEST("canceled fallback waiters do not overflow the stack") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    MemoryCacheProvider provider;
    auto cache = provider.getUse("shared"_kj, testLimits());
    auto key = kj::str("test-key");
    SpanBuilder span(nullptr);

    auto leader = cache->getWithFallback(key, span);
    KJ_ASSERT(leader.is<kj::Promise<Outcome>>());
    auto leaderCallback = kj::mv(leader.get<kj::Promise<Outcome>>()).then([](Outcome outcome) {
      KJ_ASSERT(outcome.is<FallbackDoneCallback>());
      return kj::mv(outcome.get<FallbackDoneCallback>());
    });

    constexpr size_t waiterCount = 50'000;
    for (size_t i = 0; i < waiterCount; ++i) {
      auto waiter = cache->getWithFallback(key, span);
      KJ_ASSERT(waiter.is<kj::Promise<Outcome>>());
    }

    KJ_EXPECT(cache->getStatsForTest().waiters == 0);
    KJ_EXPECT(cache->getStatsForTest().canceledWaiters == waiterCount);

    return leaderCallback.then([cache = kj::mv(cache)](FallbackDoneCallback callback) mutable {
      SpanBuilder span(nullptr);
      callback(kj::none, span);
      KJ_EXPECT(cache->getStatsForTest().inFlightFallbacks == 0);
    });
  });
}

KJ_TEST("fallback callback stores the value and survives binding destruction") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    MemoryCacheProvider provider;
    auto key = kj::str("test-key");
    SpanBuilder span(nullptr);

    // Keep a second binding alive so the shared cache outlives the first one.
    auto reader = provider.getUse("shared"_kj, testLimits());

    auto leader = [&]() {
      auto writer = provider.getUse("shared"_kj, testLimits());
      auto result = writer->getWithFallback(key, span);
      KJ_ASSERT(result.is<kj::Promise<Outcome>>());
      return kj::mv(result.get<kj::Promise<Outcome>>());
      // `writer` is destroyed here while the leader promise is still pending.
    }();

    // Completing the fallback emits trace spans, which requires running inside
    // the I/O context like the JS caller does in production.
    return leader.then([context = kj::addRef(env.context), reader = kj::mv(reader),
                           key = kj::mv(key)](Outcome outcome) mutable {
      KJ_ASSERT(outcome.is<FallbackDoneCallback>());
      return context->run([outcome = kj::mv(outcome), reader = kj::mv(reader), key = kj::mv(key)](
                              Worker::Lock&) mutable {
        SpanBuilder span(nullptr);
        outcome.get<FallbackDoneCallback>()(
            MemoryCacheUse::FallbackResult{kj::heapArray<kj::byte>({1, 2, 3}), kj::none}, span);

        auto cached = KJ_ASSERT_NONNULL(reader->getWithoutFallback(key, span));
        KJ_EXPECT(cached->asBytes() == kj::ArrayPtr<const kj::byte>({1, 2, 3}));
      });
    });
  });
}

// Records the spans of a request in the order they open, each as "name <- parent: key=type:value
// ...", with its tags, which a span delivers when it ends, in key order. The order spans end in
// depends on when each implementation of MemoryCache releases them, so it is not recorded.
class SpanRecorder final: public SpanObserver {
 public:
  SpanRecorder(kj::Vector<kj::String>& spans, kj::StringPtr parent)
      : spans(spans),
        parent(kj::str(parent)) {}

  kj::Rc<SpanObserver> newChild() override {
    return kj::rc<SpanRecorder>(spans, name);
  }
  void onOpen(kj::ConstString operationName, kj::Date) override {
    name = kj::str(operationName);
    index = spans.size();
    spans.add(kj::str(name, " <- ", parent, ": (open)"));
  }
  void onClose(kj::Date, Span::TagMap&& tags, kj::Vector<Span::Log>&&) override {
    auto entries = KJ_MAP(entry, tags) {
      KJ_SWITCH_ONEOF(entry.value) {
        KJ_CASE_ONEOF(str, kj::ConstString) {
          return kj::str(entry.key, "=str:", str);
        }
        KJ_CASE_ONEOF(b, bool) {
          return kj::str(entry.key, "=bool:", b);
        }
        KJ_CASE_ONEOF(d, double) {
          return kj::str(entry.key, "=float:", d);
        }
        KJ_CASE_ONEOF(i, int64_t) {
          return kj::str(entry.key, "=int:", i);
        }
      }
      KJ_UNREACHABLE;
    };
    std::sort(entries.begin(), entries.end());
    spans[index] = kj::str(name, " <- ", parent, ": ", kj::strArray(entries, " "));
  }

 private:
  kj::Vector<kj::String>& spans;
  kj::String parent;
  kj::String name;
  size_t index = 0;
};

class SpanRequestObserver final: public RequestObserver {
 public:
  explicit SpanRequestObserver(kj::Vector<kj::String>& spans)
      : span(kj::rc<SpanRecorder>(spans, "none"_kj), "request"_kjc) {}

  SpanParent getSpan() override {
    return SpanParent(span);
  }

 private:
  SpanBuilder span;
};

// Makes the MemoryCache that a test's script reads as `globalThis.CACHE`.
using MakeCache = kj::Function<v8::Local<v8::Value>(const TestFixture::Environment&)>;

// Runs a request of `script`, which reads `globalThis.CACHE`, and returns the spans it records.
// Runs in predictable mode, in which overwriting a span's tag logs an error, which fails the
// test. It logs only the first overwrite in the process.
kj::Array<kj::String> recordSpans(kj::StringPtr script, MakeCache makeCache) {
  setPredictableModeForTest();
  kj::Vector<kj::String> spans;
  TestFixture fixture({
    .mainModuleSource = script,
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&spans]() -> kj::Own<RequestObserver> {
    return kj::refcounted<SpanRequestObserver>(spans);
  }),
  });
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    jsg::check(env.js.v8Context()->Global()->Set(
        env.js.v8Context(), env.js.str("CACHE"_kj), makeCache(env)));
  });
  auto response = fixture.runRequest(kj::HttpMethod::GET, "http://worker/"_kj, ""_kj);
  KJ_EXPECT(response.statusCode == 200, response.body);
  return spans.releaseAsArray();
}

// A C++ MemoryCache over `provider`.
MakeCache cppCache(MemoryCacheProvider& provider, MemoryCacheLimits limits) {
  return [&provider, limits](const TestFixture::Environment& env) -> v8::Local<v8::Value> {
    auto& handler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<MemoryCache>>());
    return handler.wrap(env.js, env.js.alloc<MemoryCache>(provider.getUse(kj::none, limits)));
  };
}

// A Rust MemoryCache over `provider`.
MakeCache rustCache(MemoryCacheProvider& provider, MemoryCacheLimits limits) {
  return [&provider, limits](const TestFixture::Environment& env) -> v8::Local<v8::Value> {
    return jsg::check(::workerd::rust::jsg::maybe_local_from_ffi<v8::Value>(
        memory_cache_new(env.isolate, provider.bindForRust(kj::none, limits))));
  };
}

KJ_TEST("a fallback whose result is rejected is tagged as failed, without overwriting a tag") {
  MemoryCacheProvider provider;
  auto spans = recordSpans(R"SCRIPT(
      export default {
        async fetch() {
          const settle = (promise) => promise.then(() => {}, () => {});
          await settle(globalThis.CACHE.read("nan", () => ({ value: 1, expiration: NaN })));
          await settle(globalThis.CACHE.read("function", () => ({ value: () => {} })));
          return new Response("ok");
        }
      };
    )SCRIPT"_kj,
      cppCache(provider, testLimits()));

  kj::Vector<kj::StringPtr> fallbacks;
  for (auto& span: spans) {
    if (span.startsWith("memory_cache_fallback ")) fallbacks.add(span);
  }
  KJ_ASSERT(fallbacks.size() == 2, kj::strArray(spans, "\n"));
  KJ_EXPECT(fallbacks[0] ==
          "memory_cache_fallback <- memory_cache_read: "
          "fallback_error=str:TypeError: Expiration time must not be NaN. "
          "fallback_result_size=float:4 fallback_success=bool:false key=str:nan",
      fallbacks[0]);
  // kj::str() of the DOMException that serialization throws is empty.
  KJ_EXPECT(fallbacks[1] ==
          "memory_cache_fallback <- memory_cache_read: fallback_error=str: "
          "fallback_success=bool:false key=str:function",
      fallbacks[1]);
}

// Reads that take every path of read(), with and without a fallback, and fill the cache past
// its limits. The fallbacks of a key's reads are coalesced: the second read of "b" waits for the
// first, and the second of "c" runs its own once the first's fails.
// TODO(#7951): Add a value that cannot be serialized, once Rust JSG throws DOMExceptions. Rust
// throws a plain Error, so its fallback_error tag differs from that of C++'s DataCloneError.
constexpr auto PARITY_SCENARIO = R"SCRIPT(
  export default {
    async fetch() {
      const cache = globalThis.CACHE;
      const settle = (promise) => promise.then(() => {}, () => {});

      await cache.read("missing");
      await cache.read("a", async () => ({ value: "x".repeat(10) }));
      await cache.read("a");
      await cache.read("a", () => { throw new Error("unused"); });

      const leader = cache.read("b", async () => ({ value: 1, expiration: Date.now() + 1e6 }));
      const waiter = cache.read("b", () => ({ value: 2 }));
      await Promise.all([leader, waiter]);

      const failing = cache.read("c", async () => { throw new Error("boom"); });
      const promoted = cache.read("c", () => ({ value: "second" }));
      await settle(failing);
      await promoted;

      await settle(cache.read("d", () => { throw new TypeError("sync"); }));
      await settle(cache.read("e", () => ({ value: 1, expiration: NaN })));
      await settle(cache.read("f", () => Promise.reject("not an error")));
      await settle(cache.read("g", () => ({ value: "y".repeat(100) })));
      await settle(cache.read("h", () => ({ value: 1, expiration: 0 })));
      await cache.read("i", () => ({ value: "evicts a" }));
      return new Response("ok");
    }
  };
)SCRIPT"_kj;

constexpr MemoryCacheLimits PARITY_LIMITS{
  .maxKeys = 3,
  .maxValueSize = 64,
  .maxTotalValueSize = 1024,
};

// The memory_cache_* spans of PARITY_SCENARIO.
kj::Array<kj::String> parityScenarioSpans(
    MakeCache (*makeCache)(MemoryCacheProvider&, MemoryCacheLimits)) {
  MemoryCacheProvider provider;
  kj::Vector<kj::String> result;
  for (auto& span: recordSpans(PARITY_SCENARIO, makeCache(provider, PARITY_LIMITS))) {
    if (span.startsWith("memory_cache")) result.add(kj::mv(span));
  }
  return result.releaseAsArray();
}

KJ_TEST("the Rust MemoryCache records the spans the C++ one does") {
  auto cpp = parityScenarioSpans(cppCache);
  auto rust = parityScenarioSpans(rustCache);
  auto describe = [&]() {
    return kj::str("C++:\n", kj::strArray(cpp, "\n"), "\nRust:\n", kj::strArray(rust, "\n"));
  };
  KJ_EXPECT(cpp.size() == rust.size(), describe());
  for (auto i: kj::zeroTo(kj::min(cpp.size(), rust.size()))) {
    KJ_EXPECT(cpp[i] == rust[i], i, describe());
  }

  // The comparison means something only if C++ records what it should.
  auto contains = [&](kj::StringPtr expected) {
    for (auto& span: cpp) {
      if (span == expected) return true;
    }
    return false;
  };
  KJ_EXPECT(contains("memory_cache_read <- request: cache_entry_count=float:0 "
                     "cache_hit=bool:false cache_total_size=float:0 "
                     "memory_cache_lock_wait_time_ns=int:0"_kj),
      describe());
  KJ_EXPECT(contains("memory_cache_fallback <- memory_cache_read: fallback_error=str:Error: boom "
                     "fallback_success=bool:false key=str:c"_kj),
      describe());
}

}  // namespace
}  // namespace workerd::api
