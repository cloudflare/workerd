// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/memory.h>

#ifdef __clang__
// Marks a member function that may synchronously destroy its target when called through a kj::Ptr.
// Calls to annotated methods on kj::Ptr<T> must use consume(kj::mv(ptr))->method(...), which drops
// the active kj::Ptr before entering the method.
#define WD_CONSUME __attribute__((annotate("workerd_consume")))
#else
#define WD_CONSUME
#endif

namespace workerd {

// Wraps a kj::Ptr for a single member-function call that may synchronously destroy the target.
//
// Normally, code must not destroy a PtrTarget while a kj::Ptr to it is alive. Sometimes an object
// intentionally invokes a callback that is allowed to tear down the callback target itself. Use this
// helper only for that pattern:
//
//   KJ_IF_SOME(ptr, weak.upgrade()) {
//     consume(kj::mv(ptr))->destructionCapableCallback(...);
//   }
//
// operator->() drops the counted kj::Ptr before the member function starts, so the callee can
// destroy the target without violating PtrTarget's active-pointer contract. The returned raw pointer
// must only be used for the immediate member call expression; do not store it or call operator->()
// manually.
template <typename T>
class [[nodiscard]] ConsumePtr final {
 public:
  explicit ConsumePtr(kj::Ptr<T>&& ptr): ptr(kj::mv(ptr)) {}
  KJ_DISALLOW_COPY_AND_MOVE(ConsumePtr);

  inline T* operator->() && {
    T* result = ptr.get();
    KJ_IREQUIRE(result != nullptr);
    // Drop the counted kj::Ptr before the member call starts. The callee may synchronously destroy
    // the target, so keeping an active kj::Ptr across the call would violate PtrTarget's contract.
    ptr = nullptr;
    return result;
  }

 private:
  kj::Ptr<T> ptr;
};

template <typename T>
[[nodiscard]] inline ConsumePtr<T> consume(kj::Ptr<T>&& ptr) {
  return ConsumePtr<T>(kj::mv(ptr));
}

}  // namespace workerd
