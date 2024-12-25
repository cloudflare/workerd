use cxx::ExternType;

use crate::ffi::own_promise_node_drop_in_place;

// OwnPromiseNode

// The inner pointer is never read on Rust's side, so Rust thinks it's dead code.
#[allow(dead_code)]
pub struct OwnPromiseNode(*const ());

#[repr(transparent)]
pub struct PtrOwnPromiseNode(*mut OwnPromiseNode);

unsafe impl ExternType for OwnPromiseNode {
    type Id = cxx::type_id!("workerd::rust::async::OwnPromiseNode");
    type Kind = cxx::kind::Trivial;
}

unsafe impl ExternType for PtrOwnPromiseNode {
    type Id = cxx::type_id!("workerd::rust::async::PtrOwnPromiseNode");
    type Kind = cxx::kind::Trivial;
}

impl Drop for OwnPromiseNode {
    fn drop(&mut self) {
        // The pin crate suggests implementing drop traits for address-sensitive types with an inner
        // function which accepts a `Pin<&mut Type>` parameter, to help uphold pinning guarantees.
        // However, since our drop function is actually a C++ destructor to which we must pass a raw
        // pointer, there is no benefit in creating a Pin from `self`.
        unsafe {
            own_promise_node_drop_in_place(PtrOwnPromiseNode(self));
        }
    }
}
