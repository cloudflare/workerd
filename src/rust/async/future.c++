#include <workerd/rust/async/lib.rs.h>
#include <workerd/rust/async/future.h>

namespace workerd::rust::async {

template <>
void box_future_drop_in_place(BoxFuture<void>* self) {
  box_future_void_drop_in_place(self);
}
template <>
bool box_future_poll(BoxFuture<void>& self, const CxxWaker& waker) {
  return box_future_void_poll(self, waker);
}
template <>
bool box_future_poll_with_co_await_waker(BoxFuture<void>& self, const CoAwaitWaker& waker) {
  return box_future_void_poll_with_co_await_waker(self, waker);
}

// ---------------------------------------------------------

template <>
void box_future_drop_in_place(BoxFuture<Fallible<void>>* self) {
  box_future_fallible_void_drop_in_place(self);
}
template <>
bool box_future_poll(BoxFuture<Fallible<void>>& self, const CxxWaker& waker) {
  return box_future_fallible_void_poll(self, waker);
}
template <>
bool box_future_poll_with_co_await_waker(BoxFuture<Fallible<void>>& self, const CoAwaitWaker& waker) {
  return box_future_fallible_void_poll_with_co_await_waker(self, waker);
}

}  // namespace workerd::rust::async
