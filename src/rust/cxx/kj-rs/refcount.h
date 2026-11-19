#pragma once

#include <kj/refcount.h>

extern "C" {

// The `rc` / `arc` inputs point to Rust `kj_rs::repr::KjRc<T>` / `KjArc<T>` values, which mirror
// `kj::Rc<T>` / `kj::Arc<T>` for ordinary objects as two raw pointers: first the `T*` pointee,
// then the refcount owner. The helpers below only inspect or mutate the owner's refcount; they
// do not dereference the pointee. KJ pointer types, which are stored inline, are not supported.

// Returns whether the input `kj::Rc<T>` has more than one outstanding reference.
bool cxxbridge$kjrs$rc$is_shared(const void* rc);

// Constructs a cloned `kj::Rc<T>` into `out`. `out` points to uninitialized storage with the same
// two-pointer layout as `kj::Rc<T>`.
void cxxbridge$kjrs$rc$clone(const void* rc, void* out);

// Destroys the input `kj::Rc<T>` in place, decrementing the control pointer's refcount.
void cxxbridge$kjrs$rc$drop(void* rc);

// Returns whether the input `kj::Arc<T>` has more than one outstanding reference.
bool cxxbridge$kjrs$arc$is_shared(const void* arc);

// Constructs a cloned `kj::Arc<T>` into `out`. `out` points to uninitialized storage with the same
// two-pointer layout as `kj::Arc<T>`.
void cxxbridge$kjrs$arc$clone(const void* arc, void* out);

// Destroys the input `kj::Arc<T>` in place, decrementing the control pointer's refcount.
void cxxbridge$kjrs$arc$drop(void* arc);

// The `weak` inputs point to Rust `kj_rs::repr::KjWeakRc<T>` values, which hold the bytes of a
// `kj::WeakRc<T>`: pointers to the owner's weak cell and to the referent. The helpers read only
// the cell, as `kj::WeakRc` does, and return or store the referent pointer without dereferencing
// it.

// Constructs a `kj::WeakRc<T>` of the input `kj::Rc<T>`'s referent into `out`, which points to
// uninitialized storage with the layout of `kj::WeakRc<T>`.
void cxxbridge$kjrs$rc$downgrade(const void* rc, void* out);

// Returns the referent of the input `kj::WeakRc<T>`, or null if it has expired.
void* cxxbridge$kjrs$weak_rc$try_get(const void* weak);

// If the referent of the input `kj::WeakRc<T>` has not expired, constructs a `kj::Rc<T>` of it
// into `out`, which points to uninitialized storage with the layout of `kj::Rc<T>`, and returns
// true. Returns false otherwise, leaving `out` uninitialized.
bool cxxbridge$kjrs$weak_rc$upgrade(const void* weak, void* out);

// Constructs a cloned `kj::WeakRc<T>` into `out`, which points to uninitialized storage with the
// layout of `kj::WeakRc<T>`.
void cxxbridge$kjrs$weak_rc$clone(const void* weak, void* out);

// Destroys the input `kj::WeakRc<T>` in place, releasing its reference to the weak cell.
void cxxbridge$kjrs$weak_rc$drop(void* weak);
}
