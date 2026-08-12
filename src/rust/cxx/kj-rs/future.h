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
    void /* RustFuture::fut */* fut, const void* waker, void /* T */* ret);

// ::kj_rs::repr::DropCallback
using DropCallback = void (*)(void /* RustFuture::fut */* fut);

// ::kj_rs::repr::RustFuture & ::kj_rs::promise::RustInfallibleFuture since they both have the
// same layout.
//
// Cancellation: Destroying the enclosing FutureAwaiter calls this struct's `drop` function pointer,
// which drops the Rust Future and transitively cancels any KJ sub-promises it was .await'ing.
struct RustFuture {

  template <typename T>
  operator kj::Promise<T>() {
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

      void poll(const ::kj_rs::KjWaker& waker, ExceptionOrValue& output) noexcept {
        ::kj_rs::FuturePoller<Output> poller;
        poller.poll(
            [this, &waker](void* result) { return fut.poll(&fut, &waker, result); }, output);
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

static_assert(sizeof(RustFuture) == 4 * sizeof(std::uintptr_t), "incorrect RustFuture layout");
}  // namespace repr

}  // namespace kj_rs
