#include "deferred-proxy.h"
#include <kj/test.h>

namespace workerd::api {
namespace {

KJ_TEST("kj::Promise<DeferredProxy<T>>: early co_return implicitly fulfills outer promise") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  {
    // Implicit void co_return.
    auto coro = []() -> kj::Promise<DeferredProxy<void>> {
      co_await kj::Promise<void>(kj::READY_NOW);
    };
    auto promise = coro();
    KJ_EXPECT(promise.poll(waitScope));
    auto proxyTask = promise.wait(waitScope).proxyTask;
    KJ_EXPECT(proxyTask.poll(waitScope));
    proxyTask.wait(waitScope);
  }
  {
    // Explicit void co_return.
    auto coro = []() -> kj::Promise<DeferredProxy<void>> {
      co_return;
    };
    auto promise = coro();
    KJ_EXPECT(promise.poll(waitScope));
    auto proxyTask = promise.wait(waitScope).proxyTask;
    KJ_EXPECT(proxyTask.poll(waitScope));
    proxyTask.wait(waitScope);
  }
  {
    // Valueful co_return.
    auto coro = []() -> kj::Promise<DeferredProxy<int>> {
      co_return 123;
    };
    auto promise = coro();
    KJ_EXPECT(promise.poll(waitScope));
    auto proxyTask = promise.wait(waitScope).proxyTask;
    KJ_EXPECT(proxyTask.poll(waitScope));
    KJ_EXPECT(proxyTask.wait(waitScope) == 123);
  }
}

KJ_TEST("kj::Promise<DeferredProxy<T>>: `KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING` fulfills outer "
    "promise") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto paf2 = kj::newPromiseAndFulfiller<int>();

  auto coro = [&]() -> kj::Promise<DeferredProxy<int>> {
    co_await paf1.promise;
    KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING;
    co_return co_await paf2.promise;
  };

  auto promise = coro();

  // paf1 unfulfilled, so we don't have a DeferredProxy<T> yet.
  KJ_EXPECT(!promise.poll(waitScope));

  paf1.fulfiller->fulfill();

  KJ_EXPECT(promise.poll(waitScope));
  auto proxyTask = promise.wait(waitScope).proxyTask;

  // paf2 unfulfilled, so we don't have a T yet.
  KJ_EXPECT(!proxyTask.poll(waitScope));

  paf2.fulfiller->fulfill(123);

  KJ_EXPECT(proxyTask.poll(waitScope));
  KJ_EXPECT(proxyTask.wait(waitScope) == 123);
}

KJ_TEST("kj::Promise<DeferredProxy<T>>: unhandled exception before "
    "`KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING`") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto paf = kj::newPromiseAndFulfiller<void>();

  auto coro = [&]() -> kj::Promise<DeferredProxy<int>> {
    co_await paf.promise;
    KJ_FAIL_ASSERT("promise should have been rejected");
  };

  auto promise = coro();

  // paf unfulfilled, so we don't have a DeferredProxy<T> yet.
  KJ_EXPECT(!promise.poll(waitScope));

  paf.fulfiller->reject(KJ_EXCEPTION(FAILED, "test error"));

  KJ_EXPECT(promise.poll(waitScope));
  KJ_EXPECT_THROW_MESSAGE("test error", promise.wait(waitScope));
}

KJ_TEST("kj::Promise<DeferredProxy<T>>: unhandled exception after "
    "`KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING`") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto paf2 = kj::newPromiseAndFulfiller<int>();

  auto coro = [&]() -> kj::Promise<DeferredProxy<int>> {
    co_await paf1.promise;
    KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING;
    co_return co_await paf2.promise;
  };

  auto promise = coro();

  // paf1 unfulfilled, so we don't have a DeferredProxy<T> yet.
  KJ_EXPECT(!promise.poll(waitScope));

  paf1.fulfiller->fulfill();

  KJ_EXPECT(promise.poll(waitScope));
  auto proxyTask = promise.wait(waitScope).proxyTask;

  // paf2 unfulfilled, so we don't have a T yet.
  KJ_EXPECT(!proxyTask.poll(waitScope));

  paf2.fulfiller->reject(KJ_EXCEPTION(FAILED, "test error"));

  KJ_EXPECT(proxyTask.poll(waitScope));
  KJ_EXPECT_THROW_MESSAGE("test error", proxyTask.wait(waitScope));
}

KJ_TEST("kj::Promise<DeferredProxy<T>>: can be `co_await`ed from another coroutine") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto paf2 = kj::newPromiseAndFulfiller<int>();

  auto nestedCoro = [&]() -> kj::Promise<DeferredProxy<int>> {
    co_await paf1.promise;
    KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING;
    co_return co_await paf2.promise;
  };

  auto coro = [&]() -> kj::Promise<DeferredProxy<int>> {
    auto deferred = co_await nestedCoro();
    KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING;
    co_return co_await deferred.proxyTask;
  };

  auto promise = coro();

  // paf1 unfulfilled, so we don't have a DeferredProxy<T> yet.
  KJ_EXPECT(!promise.poll(waitScope));

  paf1.fulfiller->fulfill();

  KJ_EXPECT(promise.poll(waitScope));
  auto proxyTask = promise.wait(waitScope).proxyTask;

  // paf2 unfulfilled, so we don't have a T yet.
  KJ_EXPECT(!proxyTask.poll(waitScope));

  paf2.fulfiller->fulfill(123);

  KJ_EXPECT(proxyTask.poll(waitScope));
  KJ_EXPECT(proxyTask.wait(waitScope) == 123);
}

struct Counter {
  size_t& wind;
  size_t& unwind;
  Counter(size_t& wind, size_t& unwind): wind(wind), unwind(unwind) { ++wind; }
  ~Counter() { ++unwind; }
  KJ_DISALLOW_COPY_AND_MOVE(Counter);
};

kj::Promise<DeferredProxy<void>> cancellationTester(
    kj::Promise<void> preDeferredProxying,
    kj::Promise<void> postDeferredProxying,
    size_t& wind,
    size_t& unwind) {
  Counter preCounter(wind, unwind);
  co_await preDeferredProxying;
  KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING;
  Counter postCounter(wind, unwind);
  co_await postDeferredProxying;
};

KJ_TEST("kj::Promise<DeferredProxy<T>>: can be canceled while suspended before deferred proxying") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  size_t wind = 0, unwind = 0;

  {
    auto neverDone1 = kj::Promise<void>(kj::NEVER_DONE);
    auto neverDone2 = kj::Promise<void>(kj::NEVER_DONE);
    neverDone1 = neverDone1.attach(kj::heap<Counter>(wind, unwind));
    neverDone2 = neverDone2.attach(kj::heap<Counter>(wind, unwind));
    auto promise = cancellationTester(kj::mv(neverDone1), kj::mv(neverDone2), wind, unwind);
    KJ_EXPECT(!promise.poll(waitScope));
  }

  KJ_EXPECT(wind == 3);
  KJ_EXPECT(unwind == 3);
}

KJ_TEST("kj::Promise<DeferredProxy<T>>: can be canceled while suspended after deferred proxying") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  size_t wind = 0, unwind = 0;

  {
    auto readyNow = kj::Promise<void>(kj::READY_NOW);
    auto neverDone = kj::Promise<void>(kj::NEVER_DONE);
    readyNow = readyNow.attach(kj::heap<Counter>(wind, unwind));
    neverDone = neverDone.attach(kj::heap<Counter>(wind, unwind));
    auto promise = cancellationTester(kj::mv(readyNow), kj::mv(neverDone), wind, unwind);
    auto proxyTask = promise.wait(waitScope).proxyTask;
    KJ_EXPECT(!proxyTask.poll(waitScope));
  }

  KJ_EXPECT(wind == 4);
  KJ_EXPECT(unwind == 4);
}

}  // namespace
}  // namespace workerd::api
