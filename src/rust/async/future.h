#pragma once

#include <workerd/rust/async/waker.h>

#include <array>

namespace workerd::rust::async {

class CoAwaitWaker;

template <typename T>
class BoxFuture;

// Function templates which are explicitly specialized for each instance of BoxFuture<T>.
template <typename T>
void box_future_drop_in_place(BoxFuture<T>* self);
template <typename T>
bool box_future_poll(BoxFuture<T>& self, const CxxWaker& waker);
template <typename T>
bool box_future_poll_with_co_await_waker(BoxFuture<T>& self, const CoAwaitWaker& waker);

// A `Pin<Box<dyn Future<Output = ()>>>` owned by C++.
//
// The only way to construct a BoxFuture<T> is by returning one from a Rust function.
template <typename T>
class BoxFuture {
public:
  BoxFuture(BoxFuture&& other) noexcept: repr(other.repr) {
    other.repr = {0, 0};
  }
  ~BoxFuture() noexcept {
    if (repr != std::array<std::uintptr_t, 2>{0, 0}) {
      box_future_drop_in_place(this);
    }
  }

  // This function constructs a `std::task::Context` in Rust wrapping the given `CxxWaker`. It then
  // calls the future's `Future::poll()` trait function.
  //
  // Returns true if the future returned `Poll::Ready`, false if the future returned
  // `Poll::Pending`.
  //
  // The `poll()` overload which accepts a `const CoAwaitWaker&` exists to optimize awaiting KJ
  // Promises inside of this Future. By passing a distinct type, instead of the abstract CxxWaker,
  // Rust can recognize the Waker later when it tries to poll a KJ Promise. If the Waker it has is a
  // CoAwaitWaker associated with the current thread's event loop, it passes it to our
  // Promise-to-Future adapter class, RustPromiseAwaiter. This gives RustPromiseAwaiter access to a
  // KJ Event which, when armed, will poll the Future being `co_awaited`. Thus, arming the Event
  // takes the place of waking the Waker.
  //
  // The overload accepting a `const CxxWaker&` exists mostly to simplify testing.
  //
  // TODO(now): Figure out how to return non-unit/void values and exceptions.
  bool poll(const CxxWaker& waker) noexcept {
    return box_future_poll(*this, waker);
  }
  bool poll(const CoAwaitWaker& waker) noexcept {
    return box_future_poll_with_co_await_waker(*this, waker);
  }

  // Tell cxx-rs that this type follows Rust's move semantics, and can thus be passed across the FFI
  // boundary.
  using IsRelocatable = std::true_type;

private:
  // Match Rust's representation of a `Box<dyn Trait>`.
  std::array<std::uintptr_t, 2> repr;
};

// ---------------------------------------------------------

template <typename T>
class BoxFutureFallible: private BoxFuture<T> {
public:
  using BoxFuture<T>::BoxFuture;
  using BoxFuture<T>::poll;
  using typename BoxFuture<T>::IsRelocatable;
};

// ---------------------------------------------------------

using BoxFutureVoid = BoxFuture<void>;

// We define this the pointer typedef so that cxx-rs can associate it with the same pointer type our
// drop function uses.
using PtrBoxFutureVoid = BoxFutureVoid*;

// ---------------------------------------------------------

using BoxFutureFallibleVoid = BoxFutureFallible<void>;

// We define this the pointer typedef so that cxx-rs can associate it with the same pointer type our
// drop function uses.
using PtrBoxFutureFallibleVoid = BoxFutureFallibleVoid*;

}  // namespace workerd::rust::async
