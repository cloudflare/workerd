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
}
