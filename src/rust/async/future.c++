#include <workerd/rust/async/lib.rs.h>
#include <workerd/rust/async/future.h>

namespace workerd::rust::async {

BoxFutureVoid::BoxFutureVoid(BoxFutureVoid&& other) noexcept: repr(other.repr) {
  other.repr = {0, 0};
}

BoxFutureVoid::~BoxFutureVoid() noexcept {
  if (repr != std::array<std::uintptr_t, 2>{0, 0}) {
    box_future_void_drop_in_place(this);
  }
}

bool BoxFutureVoid::poll(const CxxWaker& waker) noexcept {
  return box_future_void_poll(*this, waker);
}

bool BoxFutureVoid::poll(const CoAwaitWaker& waker) noexcept {
  return box_future_void_poll_with_co_await_waker(*this, waker);
}

}  // namespace workerd::rust::async
