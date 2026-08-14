#include "kj-rs/executor-guarded.h"

#include <kj/debug.h>

namespace kj_rs {

namespace {

const kj::Executor* tryGetCurrentThreadExecutor() {
  // kj/async.h exposes no "maybe" variant of getCurrentThreadExecutor() (verified: only the
  // KJ_REQUIRE-ing accessor exists, and the thread-local EventLoop pointer it checks is private
  // to async.c++), so probe by catching its recoverable "No event loop is running on this
  // thread" requirement failure and treating it as "no executor". This runs on teardown paths
  // only (see requireCurrentOrTearingDown below), so the exception cost is irrelevant.
  const kj::Executor* current = nullptr;
  auto maybeException =
      kj::runCatchingExceptions([&]() { current = &kj::getCurrentThreadExecutor(); });
  // (maybeException != kj::none) <=> no live EventLoop on this thread; leave `current` null.
  (void)maybeException;
  return current;
}

}  // namespace

bool isCurrent(const kj::Executor& executor) {
  return tryGetCurrentThreadExecutor() == &executor;
}

void requireCurrent(const kj::Executor& executor, kj::LiteralStringConst message) {
  KJ_REQUIRE(isCurrent(executor), message);
}

void requireCurrentOrTearingDown(const kj::Executor& executor, kj::LiteralStringConst message) {
  const kj::Executor* current = tryGetCurrentThreadExecutor();
  KJ_REQUIRE(current == &executor || current == nullptr, message);
}

}  // namespace kj_rs
