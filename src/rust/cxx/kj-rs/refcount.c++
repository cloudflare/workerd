#include "refcount.h"

#include <new>

extern "C" {

// `kj::Rc<T>` and `kj::Arc<T>` operations used by Rust (`isShared()`, `addRef()`, destructor)
// operate only on the refcount control pointer stored in the first word of the object. They do not
// depend on, cast, or dereference the second word (`T*`). Therefore smart pointers with any T can be
// treated here as smart pointers to their refcount control types for refcount-only operations.

bool cxxbridge$kjrs$rc$is_shared(const void* rc) {
  auto refcounted = *reinterpret_cast<kj::Refcounted* const*>(rc);
  return refcounted->isShared();
}

void cxxbridge$kjrs$rc$clone(const void* rc, void* out) {
  auto typed =
      const_cast<kj::Rc<kj::Refcounted>*>(reinterpret_cast<const kj::Rc<kj::Refcounted>*>(rc));
  ::new (out) kj::Rc<kj::Refcounted>(typed->addRef());
}

void cxxbridge$kjrs$rc$drop(void* rc) {
  reinterpret_cast<kj::Rc<kj::Refcounted>*>(rc)->~Rc();
}

bool cxxbridge$kjrs$arc$is_shared(const void* arc) {
  auto refcounted = *reinterpret_cast<const kj::AtomicRefcounted* const*>(arc);
  return refcounted->isShared();
}

void cxxbridge$kjrs$arc$clone(const void* arc, void* out) {
  auto typed = const_cast<kj::Arc<kj::AtomicRefcounted>*>(
      reinterpret_cast<const kj::Arc<kj::AtomicRefcounted>*>(arc));
  ::new (out) kj::Arc<kj::AtomicRefcounted>(typed->addRef());
}

void cxxbridge$kjrs$arc$drop(void* arc) {
  reinterpret_cast<kj::Arc<kj::AtomicRefcounted>*>(arc)->~Arc();
}
}
