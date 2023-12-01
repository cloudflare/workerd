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

// A WeakRef is a weak reference to a thing. Note that because T may not itself be ref-counted,
// we cannot follow the usual pattern of a weak reference that potentially converts to a strong
// reference. Instead, intended usage looks like so:
// ```
//   kj::Own<WeakRef<Foo>> weakFoo = getWeakRefSomehow();
//
//   auto wasValid = weak->runIfAlive([](Foo& thing){
//     // Use thing
//   });
// ```
//
// TODO(cleanup): It would eventually be nice to replace kj::Own<WeakRef<T>> with a
// kj::WeakOwn<T> type with the same basic characteristics.
template <typename T>
class WeakRef final: public kj::Refcounted {
public:
  inline WeakRef(kj::Badge<T>, T& thing) : maybeThing(thing) {}

  // The use of the kj::Badge<T> in the constructor ensures that the initial instances
  // of WeakRef<T> can only be created within an instance of T. The instance T is responsible
  // for creating the initial refcounted kj::Own<WeakRef<T>>, and is responsible for calling
  // invalidate() in the destructor.

  KJ_DISALLOW_COPY_AND_MOVE(WeakRef);

  // Run the functor and return true if the context is alive, otherwise return false. Note that
  // since the `IoContext` might not be alive for any async continuation, we do not provide
  // a `kj::Maybe<IoContext&> tryGet()` function. You are expected to invoke this function
  // again in the next continuation to re-check if the `IoContext` is still around.
  template<typename F>
  inline bool runIfAlive(F&& f) const {
    KJ_IF_SOME(thing, maybeThing) {
      kj::fwd<F>(f)(thing);
      return true;
    }

    return false;
  }

  inline kj::Maybe<T&> tryGet() { return maybeThing; }
  inline kj::Own<WeakRef> addRef() { return kj::addRef(*this); }
  inline bool isValid() const { return maybeThing != kj::none; }

private:
  friend T;

  inline void invalidate() {
    maybeThing = nullptr;
  }

  kj::Maybe<T&> maybeThing;
};

}  // namespace workerd
