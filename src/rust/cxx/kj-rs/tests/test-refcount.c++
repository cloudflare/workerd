#include "test-refcount.h"

#include "kj-rs-demo/lib.rs.h"

#include <kj/common.h>
#include <kj/debug.h>

namespace kj_rs_demo {
kj::Rc<OpaqueRefcountedClass> get_rc() {
  return kj::rc<OpaqueRefcountedClass>(15);
}

kj::Arc<OpaqueAtomicRefcountedClass> get_arc() {
  return kj::arc<OpaqueAtomicRefcountedClass>(16);
}
void give_arc_back(kj::Arc<OpaqueAtomicRefcountedClass> arc) {
  kj::Arc<OpaqueAtomicRefcountedClass> ret_arc = modify_own_ret_arc(kj::mv(arc));
  KJ_ASSERT(ret_arc->getData() == 328);
}
void give_rc_back(kj::Rc<OpaqueRefcountedClass> rc) {
  kj::Rc<OpaqueRefcountedClass> ret_rc = modify_own_ret_rc(kj::mv(rc));
  KJ_ASSERT(ret_rc->getData() == 467);
}

kj::Maybe<kj::Rc<OpaqueRefcountedClass>> return_maybe_rc_some() {
  return kj::rc<OpaqueRefcountedClass>(111);
}

kj::Maybe<kj::Rc<OpaqueRefcountedClass>> return_maybe_rc_none() {
  return kj::none;
}

kj::Maybe<kj::Arc<OpaqueAtomicRefcountedClass>> return_maybe_arc_some() {
  return kj::arc<OpaqueAtomicRefcountedClass>(222);
}

kj::Maybe<kj::Arc<OpaqueAtomicRefcountedClass>> return_maybe_arc_none() {
  return kj::none;
}

void take_maybe_rc(kj::Maybe<kj::Rc<OpaqueRefcountedClass>> maybe) {
  auto& rc = KJ_ASSERT_NONNULL(maybe);
  KJ_ASSERT(rc->getData() == 111);
}

void maybe_rc_rust_driver() {
  // `kj::none` round-trips as `kj::none`.
  KJ_ASSERT(take_maybe_rc_ret(kj::none) == kj::none);

  // A populated `Maybe<Rc>` round-trips, and Rust mutates the pointee to 467.
  kj::Maybe<kj::Rc<OpaqueRefcountedClass>> result =
      take_maybe_rc_ret(kj::rc<OpaqueRefcountedClass>(15));
  auto& rc = KJ_ASSERT_NONNULL(result);
  KJ_ASSERT(rc->getData() == 467);
}
}  // namespace kj_rs_demo
