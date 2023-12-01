#pragma once

#include <kj/mutex.h>
#include <kj/refcount.h>

namespace workerd {

// Represents a weak reference back to an object that code can use as an indirect pointer when
// they want to be able to race destruction safely. A caller wishing to use a weak reference to
// the object should acquire a strong reference. It's always safe to invoke `tryAddStrongRef` to
// try to obtain a strong reference of the underlying object. This is because the Object's
// destructor will explicitly clear the underlying pointer that would be dereferenced by
// `tryAddStrongRef`. This means that after the refcount reaches 0, `tryAddStrongRef` is always
// still safe to invoke even if the underlying object memory has been deallocated (provided
// ownership of the weak object reference is retained).
// T must itself extend from kj::AtomicRefcounted
template <typename T>
class AtomicWeakRef final: public kj::AtomicRefcounted {
public:
  inline static kj::Own<const AtomicWeakRef<T>> wrap(T* this_) {
    return kj::atomicRefcounted<AtomicWeakRef<T>>(this_);
  }

  inline explicit AtomicWeakRef(T* thisArg) : this_(thisArg) {}

  // This tries to materialize a strong reference to the owner. It will fail if the owner's
  // refcount has already dropped to 0. As discussed in the class, the lifetime of this weak
  // reference can exceed the lifetime of the object it's tracking.
  inline kj::Maybe<kj::Own<const T>> tryAddStrongRef() const {
    auto lock = this_.lockShared();
    if (*lock == nullptr) return kj::none;
    return kj::atomicAddRefWeak(**lock);
  }

  inline kj::Own<const AtomicWeakRef<T>> addRef() const {
    return kj::atomicAddRef(*this);
  }

private:
  kj::MutexGuarded<const T*> this_;

  // This is invoked by the owner destructor to clear the pointer. That means that any racing
  // code will never try to invoke `atomicAddRefWeak` on the instance any more. Any code racing
  // in between the refcount dropping to 0 and the invalidation getting invoked will still fail
  // to acquire a strong reference. Any code acquiring a strong reference prior to the refcount
  // dropping to 0 will prevent invalidation until that extra reference is dropped.
  inline void invalidate() const {
    *this_.lockExclusive() = nullptr;
  }

  friend T;
};

}  // namespace workerd
