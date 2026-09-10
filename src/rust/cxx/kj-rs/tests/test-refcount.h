#pragma once

#include <kj/refcount.h>

#include <cstdint>

namespace kj_rs_demo {

class OpaqueRefcountedClass: public kj::Refcounted {
 public:
  OpaqueRefcountedClass(uint64_t data): data(data) {}
  ~OpaqueRefcountedClass() {}
  uint64_t getData() const {
    return this->data;
  }
  void setData(uint64_t val) {
    this->data = val;
  }

 private:
  uint64_t data;
};

class OpaqueAtomicRefcountedClass: public kj::AtomicRefcounted {
 public:
  OpaqueAtomicRefcountedClass(uint64_t data): data(data) {}
  ~OpaqueAtomicRefcountedClass() {}
  uint64_t getData() const {
    return this->data;
  }
  void setData(uint64_t val) {
    this->data = val;
  }

 private:
  uint64_t data;
};

kj::Rc<OpaqueRefcountedClass> get_rc();
kj::Arc<OpaqueAtomicRefcountedClass> get_arc();

void give_arc_back(kj::Arc<OpaqueAtomicRefcountedClass> arc);
void give_rc_back(kj::Rc<OpaqueRefcountedClass> rc);

// Helpers to test `kj::Maybe<kj::Rc<T>>` / `kj::Maybe<kj::Arc<T>>` over FFI.
kj::Maybe<kj::Rc<OpaqueRefcountedClass>> return_maybe_rc_some();
kj::Maybe<kj::Rc<OpaqueRefcountedClass>> return_maybe_rc_none();
kj::Maybe<kj::Arc<OpaqueAtomicRefcountedClass>> return_maybe_arc_some();
kj::Maybe<kj::Arc<OpaqueAtomicRefcountedClass>> return_maybe_arc_none();

// Asserts the `Maybe<Rc>` is set and that its pointee holds the expected data.
void take_maybe_rc(kj::Maybe<kj::Rc<OpaqueRefcountedClass>> maybe);

// Drives the Rust `take_maybe_rc_ret` function, exercising `kj::Maybe<kj::Rc>`
// as a Rust return type for both the none and some cases.
void maybe_rc_rust_driver();

}  // namespace kj_rs_demo
