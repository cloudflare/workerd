#pragma once

#include <workerd/rust/async/waker.h>

#include <array>

namespace workerd::rust::async {

class CoAwaitWaker;

template <typename T>
class BoxFuture;

// Corresponds to Result on the Rust side. Does not currently wrap an exception, though maybe it
// should.
template <typename T>
class Fallible {
public:
  template <typename... U>
  Fallible(U&&... args): value(kj::fwd<U>(args)...) {}
  operator T&() { return value; }
private:
  T value;
};

template <>
class Fallible<void> {};

template <typename T>
class BoxFutureFulfiller {
public:
  BoxFutureFulfiller(kj::_::ExceptionOr<kj::_::FixVoid<T>>& resultRef): result(resultRef) {}
  void fulfill(kj::_::FixVoid<T> value) { result.value = kj::mv(value); }
private:
  kj::_::ExceptionOr<kj::_::FixVoid<T>>& result;
};

template <>
class BoxFutureFulfiller<void> {
public:
  BoxFutureFulfiller(kj::_::ExceptionOr<kj::_::FixVoid<void>>& resultRef): result(resultRef) {}
  void fulfill(kj::_::FixVoid<void> value) { result.value = kj::mv(value); }
  // For Rust, because it doesn't know how to translate between kj::_::Void and ().
  void fulfill() { fulfill({}); }
private:
  kj::_::ExceptionOr<kj::_::FixVoid<void>>& result;
};

template <typename T>
class BoxFutureFulfiller<Fallible<T>> {
public:
  BoxFutureFulfiller(kj::_::ExceptionOr<kj::_::FixVoid<T>>& resultRef): result(resultRef) {}
  void fulfill(kj::_::FixVoid<T> value) { result.value = kj::mv(value); }
private:
  kj::_::ExceptionOr<kj::_::FixVoid<T>>& result;
};

template <>
class BoxFutureFulfiller<Fallible<void>> {
public:
  BoxFutureFulfiller(kj::_::ExceptionOr<kj::_::FixVoid<void>>& resultRef): result(resultRef) {}
  void fulfill(kj::_::FixVoid<void> value) { result.value = kj::mv(value); }
  // For Rust, because it doesn't know how to translate between kj::_::Void and ().
  void fulfill() { fulfill({}); }
private:
  kj::_::ExceptionOr<kj::_::FixVoid<void>>& result;
};

// ---------------------------------------------------------

template <typename T>
struct RemoveFallible_ {
  using Type = T;
};
template <typename T>
struct RemoveFallible_<Fallible<T>> {
  using Type = T;
};
template <typename T>
using RemoveFallible = typename RemoveFallible_<T>::Type;

// Function templates which are explicitly specialized for each instance of BoxFuture<T>.
template <typename T>
void box_future_drop_in_place(BoxFuture<T>* self);
template <typename T>
bool box_future_poll(BoxFuture<T>& self, const CxxWaker& waker, BoxFutureFulfiller<T>&);
template <typename T>
bool box_future_poll_with_co_await_waker(
    BoxFuture<T>& self, const CoAwaitWaker& waker, BoxFutureFulfiller<T>&);

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

  using ExceptionOrValue = kj::_::ExceptionOr<kj::_::FixVoid<RemoveFallible<T>>>;

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
  // TODO(now): Get rid of one of these overloads.
  bool poll(const CxxWaker& waker, ExceptionOrValue& output) noexcept {
    bool ready = false;

    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
      BoxFutureFulfiller<T> fulfiller(output);
      // Safety: BoxFutureFulfiller is effectively pinned because it's a temporary.
      ready = box_future_poll(*this, waker, fulfiller);
    })) {
      output.addException(kj::mv(exception));
      ready = true;
    }

    return ready;
  }
  bool poll(const CoAwaitWaker& waker, ExceptionOrValue& output) noexcept {
    bool ready = false;

    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
      BoxFutureFulfiller<T> fulfiller(output);
      // Safety: BoxFutureFulfiller is effectively pinned.
      ready = box_future_poll_with_co_await_waker(*this, waker, fulfiller);
    })) {
      output.addException(kj::mv(exception));
      ready = true;
    }

    return ready;
  }

  // Tell cxx-rs that this type follows Rust's move semantics, and can thus be passed across the FFI
  // boundary.
  using IsRelocatable = std::true_type;

private:
  // Match Rust's representation of a `Box<dyn Trait>`.
  std::array<std::uintptr_t, 2> repr;
};

// =======================================================================================
// Boilerplate follows

using BoxFutureVoid = BoxFuture<void>;

// We define this pointer typedef so that cxx-rs can associate it with the same pointer type our
// drop function uses.
using PtrBoxFutureVoid = BoxFutureVoid*;

using BoxFutureFulfillerVoid = BoxFutureFulfiller<void>;

// ---------------------------------------------------------

using BoxFutureFallibleVoid = BoxFuture<Fallible<void>>;

// We define this the pointer typedef so that cxx-rs can associate it with the same pointer type our
// drop function uses.
using PtrBoxFutureFallibleVoid = BoxFutureFallibleVoid*;

using BoxFutureFulfillerFallibleVoid = BoxFutureFulfiller<Fallible<void>>;

// ---------------------------------------------------------

using BoxFutureFallibleI32 = BoxFuture<Fallible<int32_t>>;

// We define this pointer typedef so that cxx-rs can associate it with the same pointer type our
// drop function uses.
using PtrBoxFutureFallibleI32 = BoxFutureFallibleI32*;

using BoxFutureFulfillerFallibleI32 = BoxFutureFulfiller<Fallible<int32_t>>;

}  // namespace workerd::rust::async
