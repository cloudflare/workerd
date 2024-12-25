use std::future::Future;
use std::future::IntoFuture;

// use std::mem::MaybeUninit;

use std::pin::Pin;

use std::task::Context;
use std::task::Poll;

use cxx::ExternType;

use crate::waker::deref_root_waker;

use crate::lazy_pin_init::LazyPinInit;

#[path = "await.h.rs"]
mod await_h;
pub use await_h::RustPromiseAwaiter;

#[repr(transparent)]
pub struct PtrRustPromiseAwaiter(*mut RustPromiseAwaiter);

use crate::ffi::rust_promise_awaiter_drop_in_place;
use crate::ffi::rust_promise_awaiter_new_in_place;

use crate::OwnPromiseNode;

impl Drop for RustPromiseAwaiter {
    fn drop(&mut self) {
        // The pin crate suggests implementing drop traits for address-sensitive types with an inner
        // function which accepts a `Pin<&mut Type>` parameter, to help uphold pinning guarantees.
        // However, since our drop function is actually a C++ destructor to which we must pass a raw
        // pointer, there is no benefit in creating a Pin from `self`.
        unsafe {
            rust_promise_awaiter_drop_in_place(PtrRustPromiseAwaiter(self));
        }
    }
}

unsafe impl ExternType for RustPromiseAwaiter {
    type Id = cxx::type_id!("workerd::rust::async::RustPromiseAwaiter");
    type Kind = cxx::kind::Opaque;
}

unsafe impl ExternType for PtrRustPromiseAwaiter {
    type Id = cxx::type_id!("workerd::rust::async::PtrRustPromiseAwaiter");
    type Kind = cxx::kind::Trivial;
}

// =======================================================================================
// Await syntax for OwnPromiseNode

impl IntoFuture for OwnPromiseNode {
    type Output = ();
    type IntoFuture = RustPromiseAwaiterFuture;

    fn into_future(self) -> Self::IntoFuture {
        RustPromiseAwaiterFuture::new(self)
    }
}

pub struct RustPromiseAwaiterFuture {
    node: Option<OwnPromiseNode>,
    awaiter: LazyPinInit<RustPromiseAwaiter>,
}

impl RustPromiseAwaiterFuture {
    fn new(node: OwnPromiseNode) -> Self {
        RustPromiseAwaiterFuture {
            node: Some(node),
            awaiter: LazyPinInit::uninit(),
        }
    }
}

impl Future for RustPromiseAwaiterFuture {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<()> {
        if let Some(root_waker) = deref_root_waker(cx.waker()) {
            // On our first invocation, `node` will be Some, and `awaiter.get()`'s callback will
            // immediately move pass its contents into the RustPromiseAwaiter constructor. On all
            // subsequent invocations, `node` will be None and the `awaiter.get()` callback will
            // not fire.
            let node = self.node.take();

            // Our awaiter is structurally pinned.
            // TODO(now): Safety comment.
            let awaiter = unsafe { self.map_unchecked_mut(|s| &mut s.awaiter) };

            let awaiter = awaiter.get(move |ptr: *mut RustPromiseAwaiter| unsafe {
                rust_promise_awaiter_new_in_place(
                    PtrRustPromiseAwaiter(ptr),
                    root_waker,
                    // `node` is consumed
                    node.expect("init function only called once"),
                );
            });

            if awaiter.poll(root_waker) {
                Poll::Ready(())
            } else {
                Poll::Pending
            }
        } else {
            unreachable!("unimplemented");
            // TODO(now): Store a clone of the waker, then replace self.node with the result
            //   of wake_after(&waker, node), which will be implemented like
            //   node.attach(kj::defer([&waker]() { waker.wake_by_ref(); }))
            //       .eagerlyEvaluate(nullptr)
        }
    }
}
