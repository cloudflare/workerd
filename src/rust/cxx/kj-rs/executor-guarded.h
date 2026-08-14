#pragma once

#include <kj/async.h>
#include <kj/debug.h>

namespace kj_rs {

// ExecutorGuarded is a helper class which allows mutable access to a wrapped value to any thread
// running the KJ event loop that was active at the time of construction. Any access attempts by a
// thread not running the original event loop are met with thrown exceptions.
template <typename T>
class ExecutorGuarded {
 public:
  template <typename... Args>
  ExecutorGuarded(Args&&... args): value(kj::fwd<Args>(args)...) {}
  ~ExecutorGuarded() noexcept(false) {
    // Teardown-tolerant: if the current thread has NO event loop at all (~EventLoop already ran),
    // proceed quietly (best-effort destruction of `value`). Destruction can legitimately run
    // after loop teardown from Rust drop glue (e.g. the tokio runtime cancelling still-pending
    // LocalSet tasks in TokioPort::drop, after TokioAsyncIoContext destroyed the WaitScope and
    // EventLoop); a throw there would unwind through a cxx `prevent_unwind` boundary and abort
    // the process. Destruction on a thread running a DIFFERENT live event loop is still a
    // contract violation and throws.
    KJ_REQUIRE(executor->isCurrent() || kj::tryGetCurrentThreadExecutor() == kj::none,
        "destruction on wrong event loop");
  }
  KJ_DISALLOW_COPY_AND_MOVE(ExecutorGuarded);

  // Check that the current thread is running this ExecutorGuarded object's original event loop,
  // then return a mutable reference to the guarded object.
  //
  // Throws an exception with `message` if the current thread is not running the expected event
  // loop.
  T& get(kj::LiteralStringConst message = "access on wrong event loop"_kjc) const {
    KJ_REQUIRE(executor->isCurrent(), message);

    // Safety: const_cast is okay because we know that we are being accessed on a thread running our
    // original event loop. All successful accesses through `get()` are effectively single-threaded,
    // even though the event loop, and this object, may collectively move between threads.
    return const_cast<T&>(value);
  }

  kj::Maybe<T&> tryGet() const {
    if (executor->isCurrent()) {
      // Safety: const_cast is okay because we know that we are being accessed on a thread running our
      // original event loop. All successful accesses through `get()` are effectively single-threaded,
      // even though the event loop, and this object, may collectively move between threads.
      return const_cast<T&>(value);
    } else {
      return kj::none;
    }
  }

 private:
  // Owned (via `addRef()`) rather than a bare `const kj::Executor&`, so the Executor object stays
  // alive as long as this guard does. A guarded value can be destroyed by Rust *after* the KJ
  // event loop has been torn down (e.g. a bridged future dropped during teardown); with a bare
  // reference the destructor's executor check would take the address of — and a reused address
  // could alias — a freed Executor. `addRef()` keeps the Executor at a stable, valid address; if
  // the loop is gone, `isCurrent()` reports false without throwing and the destructor lets
  // destruction proceed quietly (`get()` still throws, since post-teardown *access* remains a
  // contract violation).
  kj::Own<const kj::Executor> executor = kj::getCurrentThreadExecutor().addRef();
  T value;
};

}  // namespace kj_rs
