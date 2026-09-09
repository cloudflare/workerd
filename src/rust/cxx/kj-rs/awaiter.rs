#![allow(unsafe_code)]

use std::mem::MaybeUninit;
use std::pin::Pin;
use std::task::Context;

use crate::OwnPromiseNode;
// =======================================================================================
// Await syntax for OwnPromiseNode
use crate::ffi::GuardedRustPromiseAwaiter;
use crate::ffi::GuardedRustPromiseAwaiterRepr;
use crate::waker::try_poll_waker;

pub struct PromiseAwaiter<Data: std::marker::Unpin> {
    node: Option<OwnPromiseNode>,
    pub(crate) data: Data,
    awaiter: MaybeUninit<GuardedRustPromiseAwaiterRepr>,
    awaiter_initialized: bool,
    // Suppresses the auto `Unpin` impl. After the first poll, `awaiter` holds an in-place C++
    // Event and the promise node's self-pointer points into this memory. Moving `self` after that
    // would leave both pointers dangling.
    _pinned: std::marker::PhantomPinned,
}

impl<Data: std::marker::Unpin> PromiseAwaiter<Data> {
    pub fn new(node: OwnPromiseNode, data: Data) -> Self {
        Self {
            node: Some(node),
            data,
            awaiter: MaybeUninit::uninit(),
            awaiter_initialized: false,
            _pinned: std::marker::PhantomPinned,
        }
    }

    /// # Panics
    ///
    /// Panics if `node` is None.
    #[must_use]
    pub fn get_awaiter(mut self: Pin<&mut Self>) -> Pin<&mut GuardedRustPromiseAwaiter> {
        // Safety: We never move out of `this`.
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
        let this = unsafe { Pin::into_inner_unchecked(self.as_mut()) };

        // Initialize the awaiter if not already done
        if !this.awaiter_initialized {
            // On our first invocation, `node` will be Some, and `get_awaiter` will forward its
            // contents into GuardedRustPromiseAwaiter's constructor. On all subsequent invocations, `node`
            // will be None and the constructor will not run.
            let node = this.node.take();

            // Safety: The memory slot is valid and this type ensures that it will stay pinned.
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe {
                crate::ffi::guarded_rust_promise_awaiter_new_in_place(
                    this.awaiter
                        .as_mut_ptr()
                        .cast::<GuardedRustPromiseAwaiter>(),
                    node.expect("node should be Some in call to init()"),
                );
            }
            this.awaiter_initialized = true;
        }

        // Safety: `this.awaiter` is pinned since `self` is pinned.
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
        unsafe {
            let raw =
                std::ptr::from_mut::<GuardedRustPromiseAwaiterRepr>(this.awaiter.assume_init_mut());
            let raw = raw.cast::<GuardedRustPromiseAwaiter>();
            Pin::new_unchecked(&mut *raw)
        }
    }

    pub fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> bool {
        match try_poll_waker(cx.waker()) {
            Some(poll_waker) => self
                .as_mut()
                .get_awaiter()
                .poll_with_poll_waker(&WakerRef(cx.waker()), poll_waker),
            None => self.as_mut().get_awaiter().poll(&WakerRef(cx.waker())),
        }
    }
}

impl<Data: std::marker::Unpin> Drop for PromiseAwaiter<Data> {
    fn drop(&mut self) {
        if self.awaiter_initialized {
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe {
                crate::ffi::guarded_rust_promise_awaiter_drop_in_place(
                    self.awaiter
                        .as_mut_ptr()
                        .cast::<GuardedRustPromiseAwaiter>(),
                );
            }
        }
    }
}

// =======================================================================================
// WakerRef and RustWaker

pub struct WakerRef<'a>(&'a std::task::Waker);

/// An owned clone of a `std::task::Waker`, exposed to C++ as an opaque type. The C++
/// `RustPromiseAwaiter` holds its clone of the Waker it was last polled with as a
/// `kj::Maybe<rust::Box<RustWaker>>`.
pub struct RustWaker(std::task::Waker);

impl RustWaker {
    pub fn wake(&self) {
        self.0.wake_by_ref();
    }

    pub fn will_wake(&self, waker: &WakerRef) -> bool {
        self.0.will_wake(waker.0)
    }
}

pub fn clone_waker(waker: &WakerRef) -> Box<RustWaker> {
    Box::new(RustWaker(waker.0.clone()))
}
