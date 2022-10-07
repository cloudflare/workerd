// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "co-capture.h"

#include <kj/async-io.h>
#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("Verify coCapture() functors can only be run once") {
  auto io = kj::setupAsyncIo();

  auto functor = coCapture([](kj::Timer& timer) -> kj::Promise<void> {
    co_await timer.afterDelay(1 * kj::MILLISECONDS);
  });

  auto promise = functor(io.lowLevelProvider->getTimer());
  KJ_EXPECT_THROW(FAILED, functor(io.lowLevelProvider->getTimer()));

  promise.wait(io.waitScope);
}

auto makeDelayedIntegerFunctor(size_t i) {
  return [i](kj::Timer& timer) -> kj::Promise<size_t> {
    co_await timer.afterDelay(1 * kj::MILLISECONDS);
    co_return i;
  };
}

KJ_TEST("Verify coCapture() with local scoped functors") {
  auto io = kj::setupAsyncIo();

  constexpr size_t COUNT = 100;
  kj::Vector<kj::Promise<size_t>> promises;
  for (size_t i = 0; i < COUNT; ++i) {
    auto functor = coCapture(makeDelayedIntegerFunctor(i));
    promises.add(functor(io.lowLevelProvider->getTimer()));
  }

  for (size_t i = COUNT; i > 0 ; --i) {
    auto j = i-1;
    auto result = promises[j].wait(io.waitScope);
    KJ_REQUIRE(result == j);
  }
}

auto makeCheckThenDelayedIntegerFunctor(kj::Timer& timer, size_t i) {
  return [&timer, i](size_t val) -> kj::Promise<size_t> {
    KJ_REQUIRE(val == i);
    co_await timer.afterDelay(1 * kj::MILLISECONDS);
    co_return i;
  };
}

KJ_TEST("Verify coCapture() with continuation functors") {
  // This test usually works locally without `coCapture()()`. It does however, fail in
  // ASAN.
  auto io = kj::setupAsyncIo();

  constexpr size_t COUNT = 100;
  kj::Vector<kj::Promise<size_t>> promises;
  for (size_t i = 0; i < COUNT; ++i) {
    auto promise = io.lowLevelProvider->getTimer().afterDelay(1 * kj::MILLISECONDS).then([i]() {
      return i;
    });
    promise = promise.then(coCapture(
        makeCheckThenDelayedIntegerFunctor(io.lowLevelProvider->getTimer(), i)));
    promises.add(kj::mv(promise));
  }

  for (size_t i = COUNT; i > 0 ; --i) {
    auto j = i-1;
    auto result = promises[j].wait(io.waitScope);
    KJ_REQUIRE(result == j);
  }
}

}
}
