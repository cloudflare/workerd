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

bool BoxFutureVoid::poll(const RootWaker& waker) noexcept {
  return box_future_void_poll(*this, waker);
}

}  // namespace workerd::rust::async
