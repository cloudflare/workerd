#include <workerd/rust/async/lib.rs.h>
#include <workerd/rust/async/future.h>

namespace workerd::rust::async {

template <>
void box_future_drop_in_place(BoxFuture<void>* self) {
  box_future_drop_in_place_void(self);
}
template <>
bool box_future_poll(BoxFuture<void>& self, const KjWaker& waker, BoxFutureFulfiller<void>& fulfiller) {
  return box_future_poll_void(self, waker, fulfiller);
}

// ---------------------------------------------------------

template <>
void box_future_drop_in_place(BoxFuture<Fallible<void>>* self) {
  box_future_drop_in_place_fallible_void(self);
}
template <>
bool box_future_poll(BoxFuture<Fallible<void>>& self, const KjWaker& waker, BoxFutureFulfiller<Fallible<void>>& fulfiller) {
  return box_future_poll_fallible_void(self, waker, fulfiller);
}

// ---------------------------------------------------------

template <>
void box_future_drop_in_place(BoxFuture<Fallible<int32_t>>* self) {
  box_future_drop_in_place_fallible_i32(self);
}
template <>
bool box_future_poll(
    BoxFuture<Fallible<int32_t>>& self,
    const KjWaker& waker,
    BoxFutureFulfiller<Fallible<int32_t>>& fulfiller) {
  return box_future_poll_fallible_i32(self, waker, fulfiller);
}

}  // namespace workerd::rust::async
