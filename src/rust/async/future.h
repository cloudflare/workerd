#pragma once

#include <workerd/rust/async/waker.h>

#include <array>

namespace workerd::rust::async {

// A `Pin<Box<dyn Future<Output = ()>>>` owned by C++.
//
// The only way to construct a BoxFutureVoid is by returning one from a Rust function.
//
// TODO(now): Figure out how to make this a template, BoxFuture<T>.
class BoxFutureVoid {
public:
  BoxFutureVoid(BoxFutureVoid&&) noexcept;
  ~BoxFutureVoid() noexcept;

  // This function constructs a `std::task::Context` in Rust wrapping the given `KjWaker`. It
  // then calls the future's `Future::poll()` trait function.
  //
  // The reason we pass a `const KjWaker&`, and not the more generic `const CxxWaker&`, is
  // because `KjWaker` exposes an API which Rust can use to optimize awaiting KJ Promises inside
  // of this future.
  //
  // Returns true if the future returned `Poll::Ready`, false if the future returned
  // `Poll::Pending`.
  //
  // TODO(now): Figure out how to return non-unit/void values and exceptions.
  bool poll(const KjWaker& waker) noexcept;

  // Tell cxx-rs that this type follows Rust's move semantics, and can thus be passed across the FFI
  // boundary.
  using IsRelocatable = std::true_type;

private:
  // Match Rust's representation of a `Box<T>`.
  std::array<std::uintptr_t, 2> repr;
};

// We define this the pointer typedef so that cxx-rs can associate it with the same pointer type our
// drop function uses.
using PtrBoxFutureVoid = BoxFutureVoid*;

}  // namespace workerd::rust::async
