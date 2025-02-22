#pragma once

#include <workerd/rust/async/waker.h>

#include <array>

namespace workerd::rust::async {

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
struct RemoveFallible_ {
  using Type = T;
};
template <typename T>
struct RemoveFallible_<Fallible<T>> {
  using Type = T;
};
template <typename T>
using RemoveFallible = typename RemoveFallible_<T>::Type;

template <typename T>
class BoxFutureFulfiller {
public:
  BoxFutureFulfiller(kj::_::ExceptionOr<RemoveFallible<T>>& resultRef): result(resultRef) {}
  void fulfill(RemoveFallible<T> value) { result.value = kj::mv(value); }
private:
  kj::_::ExceptionOr<RemoveFallible<T>>& result;
};

template <>
class BoxFutureFulfiller<void> {
public:
  BoxFutureFulfiller(kj::_::ExceptionOr<kj::_::Void>& resultRef): result(resultRef) {}
  void fulfill(kj::_::Void value) { result.value = kj::mv(value); }
  // For Rust, which doesn't know about our kj::_::Void type.
  void fulfill() { fulfill({}); }
private:
  kj::_::ExceptionOr<kj::_::Void>& result;
};

template <>
class BoxFutureFulfiller<Fallible<void>>: public BoxFutureFulfiller<void> {};

// ---------------------------------------------------------

// Function templates which are explicitly specialized for each instance of BoxFuture<T>.
template <typename T>
void box_future_drop_in_place(BoxFuture<T>* self);
template <typename T>
bool box_future_poll(BoxFuture<T>& self, const KjWaker& waker, BoxFutureFulfiller<T>&);

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
      // Safety: We can assume that `this` is a valid pointer while we're in the destructor.
      box_future_drop_in_place(this);
    }
  }

  // We use the same output type for both fallible and infallible results.
  using ExceptionOrValue = kj::_::ExceptionOr<kj::_::FixVoid<RemoveFallible<T>>>;

  // Poll our Future with the given KjWaker. Returns true if the future returned `Poll::Ready`,
  // false if the future returned `Poll::Pending`.
  //
  // `output` will contain the result of the Future iff `poll()` returns true.
  bool poll(const KjWaker& waker, ExceptionOrValue& output) noexcept {
    bool ready = false;

    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
      BoxFutureFulfiller<T> fulfiller(output);
      // Safety: Both `*this` and `fulfiller` are accepted as `Pin<&mut ...>` in the Rust
      // implementation of `box_future_pull()`. This is safe because both effectively implements
      // Unpin, since they are non-self-referential, so it's fine if we decide to move them later.
      ready = box_future_poll(*this, waker, fulfiller);
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

}  // namespace workerd::rust::async
