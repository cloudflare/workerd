#pragma once

#include <workerd/rust/async/waker.h>

#include <array>

namespace workerd::rust::async {

class CoAwaitWaker;

// A `Pin<Box<dyn Future<Output = ()>>>` owned by C++.
//
// The only way to construct a BoxFutureVoid is by returning one from a Rust function.
//
// TODO(now): Figure out how to make this a template, BoxFuture<T>.
class BoxFutureVoid {
public:
  BoxFutureVoid(BoxFutureVoid&&) noexcept;
  ~BoxFutureVoid() noexcept;

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
  bool poll(const CxxWaker& waker) noexcept;
  bool poll(const CoAwaitWaker& waker) noexcept;

  // Tell cxx-rs that this type follows Rust's move semantics, and can thus be passed across the FFI
  // boundary.
  using IsRelocatable = std::true_type;

private:
  // Match Rust's representation of a `Box<dyn Trait>`.
  std::array<std::uintptr_t, 2> repr;
};

// We define this the pointer typedef so that cxx-rs can associate it with the same pointer type our
// drop function uses.
using PtrBoxFutureVoid = BoxFutureVoid*;

}  // namespace workerd::rust::async
