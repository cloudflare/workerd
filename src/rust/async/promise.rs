use std::pin::Pin;

use cxx::ExternType;

use crate::ffi::own_promise_node_drop_in_place;

use crate::CxxResult;
use crate::GuardedRustPromiseAwaiter;

// TODO(now): Generate boilerplate with a macro.
use crate::ffi::guarded_rust_promise_awaiter_unwrap_void;
use crate::ffi::promise_drop_in_place_void;
use crate::ffi::promise_into_own_promise_node_void;

// The inner pointer is never read on Rust's side, so Rust thinks it's dead code.
#[allow(dead_code)]
pub struct OwnPromiseNode(*const ());

// Safety: KJ Promises are not associated with threads, but with event loops at construction time.
// Therefore, they can be polled from any thread, as long as that thread has the correct event loop
// active at the time of the call to `poll()`. If the correct event loop is not active, the
// OwnPromiseNode's API will typically panic, undefined behavior could be possible. However, Rust
// doesn't have direct access to OwnPromiseNode's API. Instead, it can only use the Promise by
// having GuardedRustPromiseAwaiter consume it, and GuardedRustPromiseAwaiter implements the
// correct-executor guarantee.
unsafe impl Send for OwnPromiseNode {}

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

// Safety: We have a static_assert in promise.c++ which breaks if you change the size or alignment
// of the C++ definition of OwnPromiseNode, with a comment directing the reader to adjust the
// OwnPromiseNode definition in this .rs file.
//
// https://docs.rs/cxx/latest/cxx/trait.ExternType.html#integrating-with-bindgen-generated-types
unsafe impl ExternType for OwnPromiseNode {
    type Id = cxx::type_id!("workerd::rust::async::OwnPromiseNode");
    type Kind = cxx::kind::Trivial;
}

#[repr(transparent)]
pub struct PtrOwnPromiseNode(*mut OwnPromiseNode);

// Safety: Raw pointers are the same size in both languages.
unsafe impl ExternType for PtrOwnPromiseNode {
    type Id = cxx::type_id!("workerd::rust::async::PtrOwnPromiseNode");
    type Kind = cxx::kind::Trivial;
}

// ---------------------------------------------------------

pub trait PromiseTarget: Sized {
    fn into_own_promise_node(this: Promise<Self>) -> OwnPromiseNode;
    unsafe fn drop_in_place(this: PtrPromise<Self>);
    fn unwrap(awaiter: Pin<&mut GuardedRustPromiseAwaiter>) -> CxxResult<Self>;
}

use std::marker::PhantomData;

#[allow(dead_code)]
pub struct Promise<T: PromiseTarget>(*const (), PhantomData<T>);

// TODO(now): `where T: Send`? Do I need to do this for Future too?
unsafe impl<T: PromiseTarget> Send for Promise<T> {}

impl<T: PromiseTarget> Drop for Promise<T> {
    fn drop(&mut self) {
        // TODO(now): Safety comment.
        unsafe {
            T::drop_in_place(PtrPromise(self));
        }
    }
}

// TODO(now): Generate boilerplate with a macro.
// TODO(now): Safety comment.
unsafe impl ExternType for Promise<()> {
    type Id = cxx::type_id!("workerd::rust::async::PromiseVoid");
    type Kind = cxx::kind::Trivial;
}

#[repr(transparent)]
pub struct PtrPromise<T: PromiseTarget>(*mut Promise<T>);

// TODO(now): Generate boilerplate with a macro.
// Safety: Raw pointers are the same size in both languages.
unsafe impl ExternType for PtrPromise<()> {
    type Id = cxx::type_id!("workerd::rust::async::PtrPromiseVoid");
    type Kind = cxx::kind::Trivial;
}

// TODO(now): Generate boilerplate with a macro.
impl PromiseTarget for () {
    fn into_own_promise_node(this: Promise<Self>) -> OwnPromiseNode {
        promise_into_own_promise_node_void(this)
    }
    unsafe fn drop_in_place(this: PtrPromise<Self>) {
        promise_drop_in_place_void(this);
    }
    fn unwrap(
        awaiter: Pin<&mut GuardedRustPromiseAwaiter>,
    ) -> std::result::Result<Self, cxx::Exception> {
        guarded_rust_promise_awaiter_unwrap_void(awaiter)
    }
}
