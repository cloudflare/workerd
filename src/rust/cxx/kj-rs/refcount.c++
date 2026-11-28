#include "refcount.h"

#include <new>

extern "C" {

// For ordinary objects, `kj::Rc<T>` and `kj::Arc<T>` store the pointee first and the refcount
// owner second. Cloning and destruction only change the owner's refcount, preserving the pointee
// without accessing it. Erasing T to byte avoids imposing any alignment or inheritance requirement
// on the pointee, which may be a projection into an unrelated owner.

bool cxxbridge$kjrs$rc$is_shared(const void* rc) {
  auto refcounted = reinterpret_cast<kj::Refcounted* const*>(rc)[1];
  return refcounted->isShared();
}

void cxxbridge$kjrs$rc$clone(const void* rc, void* out) {
  auto typed = const_cast<kj::Rc<kj::byte>*>(reinterpret_cast<const kj::Rc<kj::byte>*>(rc));
  ::new (out) kj::Rc<kj::byte>(typed->addRef());
}

void cxxbridge$kjrs$rc$drop(void* rc) {
  reinterpret_cast<kj::Rc<kj::byte>*>(rc)->~Rc();
}

bool cxxbridge$kjrs$arc$is_shared(const void* arc) {
  auto refcounted = reinterpret_cast<const kj::AtomicRefcounted* const*>(arc)[1];
  return refcounted->isShared();
}

void cxxbridge$kjrs$arc$clone(const void* arc, void* out) {
  auto typed = reinterpret_cast<const kj::Arc<kj::byte>*>(arc);
  ::new (out) kj::Arc<kj::byte>(typed->addRef());
}

void cxxbridge$kjrs$arc$drop(void* arc) {
  reinterpret_cast<kj::Arc<kj::byte>*>(arc)->~Arc();
}

// A `kj::WeakRc<T>` holds the owner's weak cell and the referent pointer, and reaches the owner
// only through the cell, so the same erasure applies.
static_assert(sizeof(kj::WeakRc<kj::byte>) == 2 * sizeof(void*));
static_assert(alignof(kj::WeakRc<kj::byte>) == alignof(void*));

void cxxbridge$kjrs$rc$downgrade(const void* rc, void* out) {
  // downgrade() only adds a reference to the owner's weak cell, which it may allocate.
  auto typed = const_cast<kj::Rc<kj::byte>*>(reinterpret_cast<const kj::Rc<kj::byte>*>(rc));
  ::new (out) kj::WeakRc<kj::byte>(typed->downgrade());
}

void* cxxbridge$kjrs$weak_rc$try_get(const void* weak) {
  KJ_IF_SOME(referent, reinterpret_cast<const kj::WeakRc<kj::byte>*>(weak)->tryGet()) {
    return &referent;
  }
  return nullptr;
}

bool cxxbridge$kjrs$weak_rc$upgrade(const void* weak, void* out) {
  KJ_IF_SOME(rc, reinterpret_cast<const kj::WeakRc<kj::byte>*>(weak)->upgrade()) {
    ::new (out) kj::Rc<kj::byte>(kj::mv(rc));
    return true;
  }
  return false;
}

void cxxbridge$kjrs$weak_rc$clone(const void* weak, void* out) {
  // clone() only adds a reference to the shared cell.
  auto typed =
      const_cast<kj::WeakRc<kj::byte>*>(reinterpret_cast<const kj::WeakRc<kj::byte>*>(weak));
  ::new (out) kj::WeakRc<kj::byte>(typed->clone());
}

void cxxbridge$kjrs$weak_rc$drop(void* weak) {
  reinterpret_cast<kj::WeakRc<kj::byte>*>(weak)->~WeakRc();
}
}
