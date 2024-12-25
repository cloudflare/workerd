#pragma once

#include <kj/refcount.h>

namespace workerd::rust::async {

// Consume a `kj::Arc<T>` such that its destructor never runs, then return the pointer it owned. You
// must arrange to  call `unleak()` on the returned pointer later if you want to destroy it.
template <typename T>
T* leak(kj::Arc<T> value);

// Given a pointer to an `kj::AtomicRefcounted` which has previously been returned by `leak()`,
// reassume ownership by wrapping the pointer in a `kj::Arc<T>`.
template <typename T>
kj::Arc<T> unleak(T* ptr);

// =======================================================================================
// Implementation details

template <typename T>
T* leak(kj::Arc<T> value) {
  // Unions do not run non-trivial constructors or destructors for their members, unless the union's
  // own constructor/destructor are explicitly written to do so. Here, we run kj::Arc<T>'s move
  // constructor, but not its destructor, causing it to leak.
  //
  // TODO(cleanup): libkj _almost_ has a way to do this: if it were possible to cast
  //   AtomicRefcounted objects to their private Disposer base class, we could use
  //  `value.toOwn().disown(disposer)`.
  union Leak {
    kj::Arc<T> value;
    Leak(kj::Arc<T> value): value(kj::mv(value)) {}
    ~Leak() {}
  } leak{kj::mv(value)};
  return leak.value.get();
}

// HACK: We partially specialize kj::Arc<Unleak<T>> below in order to gain fraudulent friend access
//   kj::Arc<T>'s ownership-assuming constructor.
template <typename T>
struct Unleak {};

// Take a pointer to a kj::AtomicRefcounted value and assume ownership of it.
template <typename T>
kj::Arc<T> unleak(T* ptr) {
  return kj::Arc<Unleak<T>>::unleak(ptr);
}

}  // namespace workerd::rust::async

namespace kj {

template <typename T>
class Arc<workerd::rust::async::Unleak<T>> {
public:
  static_assert(kj::canConvert<T&, const AtomicRefcounted&>());
  static Arc<T> unleak(T* ptr) {
    // This unary pointer-accepting constructor seems to be the easiest way to assume ownership of a
    // raw kj::AtomicRefcounted pointer. It is private, which is why we specialized Arc<Unleak<T>>
    // in order to gain friend access.
    return Arc<T>{ptr};
  }
};

}  // namespace kj
