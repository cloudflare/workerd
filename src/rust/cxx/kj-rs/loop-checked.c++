#include "loop-checked.h"

#include <kj/debug.h>

#include <type_traits>

namespace kj_rs {

// If these fire, update `EventLoopId` in loop_checked.rs to match.
static_assert(sizeof(EventLoopId) == sizeof(size_t), "EventLoopId size changed");
static_assert(alignof(EventLoopId) == alignof(size_t), "EventLoopId alignment changed");
static_assert(std::is_trivially_copyable_v<EventLoopId>, "EventLoopId is not trivially copyable");

EventLoopId event_loop_id_current() {
  return kj::EventLoop::Id::current();
}

bool event_loop_id_is_current(EventLoopId id) {
  return kj::tryGetCurrentThreadExecutor() != kj::none && kj::EventLoop::Id::current() == id;
}

void event_loop_id_wrong_loop() {
  KJ_FAIL_REQUIRE("value used off its KJ event loop");
}

namespace {
[[noreturn]] void failDroppedOffLoop() noexcept {
  // Since the function is noexcept, this aborts the process.
  KJ_FAIL_REQUIRE("value dropped off its KJ event loop");
}
}  // namespace

void loop_checked_dropped_off_loop() {
  failDroppedOffLoop();
}

void require_async_destructors_allowed() {
  kj::AsyncObject probe;
}

}  // namespace kj_rs
