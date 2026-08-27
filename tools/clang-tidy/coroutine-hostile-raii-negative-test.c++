// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <coroutine>

namespace kj {

class UnwindDetector {
 public:
  UnwindDetector();
  ~UnwindDetector();
};

}  // namespace kj

struct Task {
  struct promise_type {
    Task get_return_object();
    std::suspend_never initial_suspend();
    std::suspend_never final_suspend() noexcept;
    void return_void();
    void unhandled_exception();
  };
};

Task detectorDestroyedBeforeAwait() {
  { kj::UnwindDetector detector; }
  co_await std::suspend_always{};
}

Task detectorCreatedAfterAwait() {
  co_await std::suspend_always{};
  kj::UnwindDetector detector;
}

Task noAwait() {
  kj::UnwindDetector detector;
  co_return;
}
