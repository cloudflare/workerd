#pragma once

#include "kj-rs/awaiter.h"
#include "kj-rs/waker.h"

#include <kj/debug.h>

#include <concepts>
#include <cstdint>

namespace kj_rs {

// Tri-state returned from `box_future_poll()`, indicating the state of its output parameter.
//
// Serves the same purpose as `cxx-async`'s FuturePollStatus:
// https://github.com/pcwalton/cxx-async/blob/ac98030dd6e5090d227e7fadca13ec3e4b4e7be7/cxx-async/include/rust/cxx_async.h#L422
enum class FuturePollStatus : uint8_t {
  // `box_future_poll()` returns Pending to indicate it did not write anything to its output
  // parameter.
  Pending,
  // `box_future_poll()` returns Complete to indicate it wrote a value to its output
  // parameter.
  Complete,
  // `box_future_poll()` returns Error to indicate it wrote an error to its output parameter.
  Error,
};

// A class with space for a `T` or a `rust::String`, whichever is larger.
template <typename T>
class FuturePoller {
 public:
  FuturePoller() {}
  ~FuturePoller() noexcept(false) {}

  // Call `pollFunc()` with a pointer to space to which a `T` (successful result) or a
  // `rust::String` (error result) may be written, then propagate the result or error to `output`
  // depending on the return value of `pollFunc()`.
  template <typename F>
  void poll(F&& pollFunc, kj::_::ExceptionOr<T>& output) {
    switch (pollFunc(&result)) {
      case ::kj_rs::FuturePollStatus::Pending:
        return;
      case ::kj_rs::FuturePollStatus::Complete: {
        output.value = kj::mv(result);
        kj::dtor(result);
        return;
      }
      case ::kj_rs::FuturePollStatus::Error: {
        output.addException(kj::mv(*error));
        delete error;
        return;
      }
    }

    KJ_UNREACHABLE;
  }

 private:
  union {
    T result;
    kj::Exception* error;
  };
};

// These types are shared with Rust code.
namespace repr {

// ::kj_rs::repr::PollCallback
using PollCallback = kj_rs::FuturePollStatus (*)(
    void /* RustFuture::fut */* fut, const ::kj_rs::PollWaker& waker, void /* T */* ret);

// ::kj_rs::repr::DropCallback
using DropCallback = void (*)(void /* RustFuture::fut */* fut);

// Rust is compiled without CFI as of writing, so we need to disable cfi-icall checks here as Rust-
// defined function are being called indirectly here.
#pragma clang attribute push(__attribute__((no_sanitize("cfi-icall"))), apply_to = function)

// ::kj_rs::repr::RustFuture & ::kj_rs::promise::RustInfallibleFuture since they both have the
// same layout.
//
// Cancellation: Destroying the enclosing FutureAwaiter calls this struct's `drop` function pointer,
// which drops the Rust Future and transitively cancels any KJ sub-promises it was .await'ing.
struct RustFuture {

  // Eager-by-default conversion: the returned promise starts running immediately, without
  // being awaited — the future is polled synchronously up to its first suspension point
  // (exactly like calling a KJ coroutine, which runs to its first co_await), and continues
  // on the event loop from there. KJ code universally assumes promises are "hot" (a stored
  // promise still makes progress), so this is the right default for every bridged
  // `async fn`; before this conversion was eager, every consumer had to remember a manual
  // `.eagerlyEvaluate(nullptr)`.
  //
  // Requires a current kj::EventLoop on this thread (same requirement as awaiting the
  // promise, just enforced at creation). Cancellation is unchanged: dropping the promise
  // still synchronously cancels the Rust future and everything it is awaiting.
  //
  // The rare consumer that genuinely wants a cold future can call `lazily()` below on the
  // raw RustFuture instead of going through this conversion (the bridge's generated shims
  // always convert eagerly, so that consumer must obtain the RustFuture itself).
  template <typename T>
  operator kj::Promise<T>() {
    return lazily<T>().eagerlyEvaluate(nullptr);
  }

  // Lazy (cold) conversion: nothing runs until the returned promise is first awaited.
  // This is the raw adapter the eager conversion above builds on, and the C++-side escape
  // hatch for code that genuinely needs a cold promise.
  template <typename T>
  kj::Promise<T> lazily() {
    struct Impl {
      using ExceptionOrValue = ::kj::_::ExceptionOr<::kj::_::FixVoid<T>>;
      using Output = ::kj::_::FixVoid<T>;

      Impl(RustFuture fut): fut(fut) {}

      ~Impl() {
        if (fut.repr != std::array<std::uintptr_t, 2>{}) {
          fut.drop(&fut);
        }
      }
      Impl(Impl&& other) {
        KJ_ASSERT(other.fut.repr != (std::array<std::uintptr_t, 2>{}));
        fut = other.fut;
        other.fut.repr = {};
      }

      KJ_DISALLOW_COPY(Impl);

      void poll(const ::kj_rs::PollWaker& waker, ExceptionOrValue& output) noexcept {
        ::kj_rs::FuturePoller<Output> poller;
        poller.poll([this, &waker](void* result) { return fut.poll(&fut, waker, result); }, output);
      }

      RustFuture fut;
    };

    return kj::_::PromiseNode::to<kj::Promise<T>>(
        kj::_::allocPromise<FutureAwaiter<Impl>>(Impl(*this)));
  }

  ::std::array<std::uintptr_t, 2> repr;
  PollCallback poll;
  DropCallback drop;
};
#pragma clang attribute pop

static_assert(sizeof(RustFuture) == 4 * sizeof(std::uintptr_t), "incorrect RustFuture layout");
}  // namespace repr

}  // namespace kj_rs
