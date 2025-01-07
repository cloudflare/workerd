use std::future::Future;
use std::future::IntoFuture;

// use std::mem::MaybeUninit;

use std::pin::Pin;

use std::task::Context;
use std::task::Poll;

use cxx::ExternType;

use crate::waker::deref_root_waker;

use crate::lazy_pin_init::LazyPinInit;

use crate::ffi::RootWaker;

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
        // Pin safety:
        // The pin crate suggests implementing drop traits for address-sensitive types with an inner
        // function which accepts a `Pin<&mut Type>` parameter, to help uphold pinning guarantees.
        // However, since our drop function is actually a C++ destructor to which we must pass a raw
        // pointer, there is no benefit in creating a Pin from `self`.
        //
        // https://doc.rust-lang.org/std/pin/index.html#implementing-drop-for-types-with-address-sensitive-states
        //
        // Pointer safety:
        // 1. Pointer to self is non-null, and obviously points to valid memory.
        // 2. We do not read or write to the OwnPromiseNode's memory, so there are no atomicity nor
        //    interleaved pointer/reference access concerns.
        //
        // https://doc.rust-lang.org/std/ptr/index.html#safety
        unsafe {
            rust_promise_awaiter_drop_in_place(PtrRustPromiseAwaiter(self));
        }
    }
}

// TODO(now): bindgen to guarantee safety
unsafe impl ExternType for RustPromiseAwaiter {
    type Id = cxx::type_id!("workerd::rust::async::RustPromiseAwaiter");
    type Kind = cxx::kind::Opaque;
}

// TODO(now): bindgen to guarantee safety
unsafe impl ExternType for PtrRustPromiseAwaiter {
    type Id = cxx::type_id!("workerd::rust::async::PtrRustPromiseAwaiter");
    type Kind = cxx::kind::Trivial;
}

// =======================================================================================
// Await syntax for OwnPromiseNode

impl IntoFuture for OwnPromiseNode {
    type Output = ();
    type IntoFuture = LazyRustPromiseAwaiter;

    fn into_future(self) -> Self::IntoFuture {
        LazyRustPromiseAwaiter::new(self)
    }
}

pub struct LazyRustPromiseAwaiter {
    node: Option<OwnPromiseNode>,
    awaiter: LazyPinInit<RustPromiseAwaiter>,
}

impl LazyRustPromiseAwaiter {
    fn new(node: OwnPromiseNode) -> Self {
        LazyRustPromiseAwaiter {
            node: Some(node),
            awaiter: LazyPinInit::uninit(),
        }
    }

    fn get_awaiter(
        self: Pin<&mut Self>,
        root_waker: &RootWaker,
        node: Option<OwnPromiseNode>,
    ) -> Pin<&mut RustPromiseAwaiter> {
        // Safety:
        // 1. We do not implment Unpin for LazyRustPromiseAwaiter.
        // 2. Our Drop trait implementation does not move the awaiter value, nor do we use
        //    `repr(packed)` anywhere.
        // 3. The backing memory is inside our pinned Future, so we can be assured our Drop trait
        //    implementation will run before Rust re-uses the memory.
        //
        // https://doc.rust-lang.org/std/pin/index.html#choosing-pinning-to-be-structural-for-field
        let awaiter = unsafe { self.map_unchecked_mut(|s| &mut s.awaiter) };

        // Safety:
        // 1. We trust that LazyPinInit's implementation passed us a valid pointer to an
        //    uninitialized RustPromiseAwaiter.
        // 2. We do not read or write to the RustPromiseAwaiter's memory, so there are no atomicity
        //    nor interleaved pointer reference access concerns.
        //
        // https://doc.rust-lang.org/std/ptr/index.html#safety
        awaiter.get_or_init(move |ptr: *mut RustPromiseAwaiter| unsafe {
            rust_promise_awaiter_new_in_place(
                PtrRustPromiseAwaiter(ptr),
                root_waker,
                node.expect("node should be Some in call to init()"),
            );
        })
    }
}

impl Future for LazyRustPromiseAwaiter {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<()> {
        if let Some(root_waker) = deref_root_waker(cx.waker()) {
            // On our first invocation, `node` will be Some, and `get_awaiter` will forward its
            // contents into RustPromiseAwaiter's constructor. On all subsequent invocations, `node`
            // will be None and the constructor will not run.
            let node = self.node.take();
            let awaiter = self.get_awaiter(root_waker, node);
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
