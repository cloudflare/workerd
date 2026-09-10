#pragma once

#include <kj/async.h>

namespace kj_rs {

// Return true if `executor`'s event loop is active on the current thread.
bool isCurrent(const kj::Executor& executor);
// Assert that `executor`'s event loop is active on the current thread, or throw an exception
// containing `message`.
void requireCurrent(const kj::Executor& executor, kj::LiteralStringConst message);

// ExecutorGuarded is a helper class which allows mutable access to a wrapped value to any thread
// running the KJ event loop that was active at the time of construction. Any access attempts by a
// thread not running the original event loop are met with thrown exceptions.
template <typename T>
class ExecutorGuarded {
 public:
  template <typename... Args>
  ExecutorGuarded(Args&&... args): value(kj::fwd<Args>(args)...) {}
  ~ExecutorGuarded() noexcept(false) {
    requireCurrent(executor, "destruction on wrong event loop"_kjc);
  }
  KJ_DISALLOW_COPY_AND_MOVE(ExecutorGuarded);

  // Check that the current thread is running this ExecutorGuarded object's original event loop,
  // then return a mutable reference to the guarded object.
  //
  // Throws an exception with `message` if the current thread is not running the expected event
  // loop.
  T& get(kj::LiteralStringConst message = "access on wrong event loop"_kjc) const {
    requireCurrent(executor, message);

    // Safety: const_cast is okay because we know that we are being accessed on a thread running our
    // original event loop. All successful accesses through `get()` are effectively single-threaded,
    // even though the event loop, and this object, may collectively move between threads.
    return const_cast<T&>(value);
  }

  kj::Maybe<T&> tryGet() const {
    if (isCurrent(executor)) {
      // Safety: const_cast is okay because we know that we are being accessed on a thread running our
      // original event loop. All successful accesses through `get()` are effectively single-threaded,
      // even though the event loop, and this object, may collectively move between threads.
      return const_cast<T&>(value);
    } else {
      return kj::none;
    }
  }

 private:
  const kj::Executor& executor = kj::getCurrentThreadExecutor();
  T value;
};

}  // namespace kj_rs
