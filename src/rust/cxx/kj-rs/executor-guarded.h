#pragma once

#include <kj/async.h>

namespace kj_rs {

// Return true if `executor`'s event loop is active on the current thread. Never throws: a
// thread with no live event loop at all (e.g. after ~EventLoop during teardown) reports false.
bool isCurrent(const kj::Executor& executor);
// Assert that `executor`'s event loop is active on the current thread, or throw an exception
// containing `message`.
void requireCurrent(const kj::Executor& executor, kj::LiteralStringConst message);
// Like requireCurrent(), but tolerant of full loop teardown: if the current thread has NO event
// loop at all (~EventLoop already ran), return quietly instead of throwing. Used by
// ~ExecutorGuarded, which can legitimately run after loop teardown from Rust drop glue (e.g. the
// tokio runtime cancelling still-pending LocalSet tasks in TokioPort::drop, after
// TokioAsyncIoContext destroyed the WaitScope and EventLoop); a throw there would unwind through
// a cxx `prevent_unwind` boundary and abort the process. Destruction on a thread running a
// DIFFERENT live event loop is still a contract violation and throws.
void requireCurrentOrTearingDown(const kj::Executor& executor, kj::LiteralStringConst message);

// ExecutorGuarded is a helper class which allows mutable access to a wrapped value to any thread
// running the KJ event loop that was active at the time of construction. Any access attempts by a
// thread not running the original event loop are met with thrown exceptions.
template <typename T>
class ExecutorGuarded {
 public:
  template <typename... Args>
  ExecutorGuarded(Args&&... args): value(kj::fwd<Args>(args)...) {}
  ~ExecutorGuarded() noexcept(false) {
    // Teardown-tolerant (see requireCurrentOrTearingDown above): destruction with no event loop
    // on the thread proceeds quietly (best-effort destruction of `value`); destruction on a
    // thread running a different live loop still throws.
    requireCurrentOrTearingDown(*executor, "destruction on wrong event loop"_kjc);
  }
  KJ_DISALLOW_COPY_AND_MOVE(ExecutorGuarded);

  // Check that the current thread is running this ExecutorGuarded object's original event loop,
  // then return a mutable reference to the guarded object.
  //
  // Throws an exception with `message` if the current thread is not running the expected event
  // loop.
  T& get(kj::LiteralStringConst message = "access on wrong event loop"_kjc) const {
    requireCurrent(*executor, message);

    // Safety: const_cast is okay because we know that we are being accessed on a thread running our
    // original event loop. All successful accesses through `get()` are effectively single-threaded,
    // even though the event loop, and this object, may collectively move between threads.
    return const_cast<T&>(value);
  }

  kj::Maybe<T&> tryGet() const {
    if (isCurrent(*executor)) {
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
  // the loop is gone, `isCurrent` reports false without throwing and the destructor's
  // `requireCurrentOrTearingDown` lets destruction proceed quietly (`get()`/`requireCurrent`
  // still throw, since post-teardown *access* remains a contract violation).
  kj::Own<const kj::Executor> executor = kj::getCurrentThreadExecutor().addRef();
  T value;
};

}  // namespace kj_rs
