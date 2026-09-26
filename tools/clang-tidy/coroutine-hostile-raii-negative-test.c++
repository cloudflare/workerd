// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <coroutine>

namespace kj {
struct UnwindDetector {};
}  // namespace kj

struct AllowedYieldable {};

struct Task {
  struct promise_type {
    Task get_return_object();
    std::suspend_never initial_suspend() noexcept;
    std::suspend_never final_suspend() noexcept;
    void return_void();
    void unhandled_exception();
    std::suspend_never yield_value(AllowedYieldable);
  };
};

struct Awaitable {
  bool await_ready();
  void await_suspend(std::coroutine_handle<>);
  int await_resume();
};

Task detectorDestroyedBeforeSuspension(Awaitable awaitable) {
  { kj::UnwindDetector detector; }
  auto result = co_await awaitable;
  (void)result;
}

void detectorOutsideCoroutine(Awaitable awaitable) {
  kj::UnwindDetector detector;
  auto coroutine = [awaitable]() mutable -> Task {
    auto result = co_await awaitable;
    (void)result;
  };
  (void)coroutine;
}

Task allowedYield() {
  kj::UnwindDetector detector;
  co_yield AllowedYieldable{};
}
