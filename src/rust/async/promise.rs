use cxx::ExternType;

use crate::ffi::own_promise_node_drop_in_place;

// The inner pointer is never read on Rust's side, so Rust thinks it's dead code.
#[allow(dead_code)]
pub struct OwnPromiseNode(*const ());

#[repr(transparent)]
pub struct PtrOwnPromiseNode(*mut OwnPromiseNode);

// TODO(now): bindgen to guarantee safety
unsafe impl ExternType for OwnPromiseNode {
    type Id = cxx::type_id!("workerd::rust::async::OwnPromiseNode");
    type Kind = cxx::kind::Trivial;
}

// TODO(now): bindgen to guarantee safety
unsafe impl ExternType for PtrOwnPromiseNode {
    type Id = cxx::type_id!("workerd::rust::async::PtrOwnPromiseNode");
    type Kind = cxx::kind::Trivial;
}

impl Drop for OwnPromiseNode {
    fn drop(&mut self) {
        // Safety:
        // 1. Pointer to self is non-null, and obviously points to valid memory.
        // 2. We do not read or write to the OwnPromiseNode's memory, so there are no atomicity nor
        //    interleaved pointer/reference access concerns.
        //
        // https://doc.rust-lang.org/std/ptr/index.html#safety
        unsafe {
            own_promise_node_drop_in_place(PtrOwnPromiseNode(self));
        }
    }
}
