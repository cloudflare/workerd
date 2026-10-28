// Tests for the C++ side of the memory cache: binding lifetime, singleflight
// leader/waiter handoff, and the FallbackDoneCallback contract. The storage and
// coordination logic itself is covered by the Rust crate's own tests.

#include "memory-cache.h"

#include <workerd/io/trace.h>
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

// Records the spans of a request as they end, each as "name <- parent: key=type:value ...", with
// its tags in key order.
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
    spans.add(kj::str(name, " <- ", parent, ": ", kj::strArray(entries, " ")));
  }

 private:
  kj::Vector<kj::String>& spans;
  kj::String parent;
  kj::String name;
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

KJ_TEST("a fallback whose result is rejected is tagged as failed, without overwriting a tag") {
  // In predictable mode, overwriting a span's tag logs an error, which fails this test. It logs
  // only the first overwrite in the process, so no other test here may overwrite one.
  setPredictableModeForTest();
  MemoryCacheProvider provider;
  kj::Vector<kj::String> spans;
  TestFixture fixture({
    .mainModuleSource = R"SCRIPT(
      export default {
        async fetch() {
          const settle = (promise) => promise.then(() => {}, () => {});
          await settle(globalThis.CACHE.read("nan", () => ({ value: 1, expiration: NaN })));
          await settle(globalThis.CACHE.read("function", () => ({ value: () => {} })));
          return new Response("ok");
        }
      };
    )SCRIPT"_kj,
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&spans]() -> kj::Own<RequestObserver> {
    return kj::refcounted<SpanRequestObserver>(spans);
  }),
  });
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& handler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<MemoryCache>>());
    auto cache =
        handler.wrap(env.js, env.js.alloc<MemoryCache>(provider.getUse(kj::none, testLimits())));
    jsg::check(
        env.js.v8Context()->Global()->Set(env.js.v8Context(), env.js.str("CACHE"_kj), cache));
  });
  auto response = fixture.runRequest(kj::HttpMethod::GET, "http://worker/"_kj, ""_kj);
  KJ_EXPECT(response.statusCode == 200, response.body);

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

}  // namespace
}  // namespace workerd::api
