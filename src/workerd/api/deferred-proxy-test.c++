#include "util.h"
#include <kj/test.h>

namespace workerd::api {
namespace {

KJ_TEST("DeferredProxyPromise<T>: early co_return implicitly fulfills outer promise") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  {
    // Implicit void co_return.
    auto coro = []() -> DeferredProxyPromise<void> {
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
    auto coro = []() -> DeferredProxyPromise<void> {
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
    auto coro = []() -> DeferredProxyPromise<int> {
      co_return 123;
    };
    auto promise = coro();
    KJ_EXPECT(promise.poll(waitScope));
    auto proxyTask = promise.wait(waitScope).proxyTask;
    KJ_EXPECT(proxyTask.poll(waitScope));
    KJ_EXPECT(proxyTask.wait(waitScope) == 123);
  }
}

KJ_TEST("DeferredProxyPromise<T>: `co_yield BEGIN_DEFERRED_PROXYING` fulfills outer promise") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto paf2 = kj::newPromiseAndFulfiller<int>();

  auto coro = [&]() -> DeferredProxyPromise<int> {
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

KJ_TEST("DeferredProxyPromise<T>: unhandled exception before `co_yield BEGIN_DEFERRED_PROXYING`") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto paf = kj::newPromiseAndFulfiller<void>();

  auto coro = [&]() -> DeferredProxyPromise<int> {
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

KJ_TEST("DeferredProxyPromise<T>: unhandled exception after `co_yield BEGIN_DEFERRED_PROXYING`") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto paf2 = kj::newPromiseAndFulfiller<int>();

  auto coro = [&]() -> DeferredProxyPromise<int> {
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

KJ_TEST("DeferredProxyPromise<T>: can be `co_await`ed from another coroutine") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto paf2 = kj::newPromiseAndFulfiller<int>();

  auto nestedCoro = [&]() -> DeferredProxyPromise<int> {
    co_await paf1.promise;
    KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING;
    co_return co_await paf2.promise;
  };

  auto coro = [&]() -> DeferredProxyPromise<int> {
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

}  // namespace
}  // namespace workerd::api
