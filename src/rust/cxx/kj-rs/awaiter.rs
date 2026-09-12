//! FFI island (see crate-root `#![deny(unsafe_code)]`): placement bridge for the C++
//! `GuardedRustPromiseAwaiter` and the `.await` poll glue — Pin projection and placement
//! new/drop over Rust-owned memory. A genuine unsafe seam.
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
    // Suppresses the auto `Unpin` impl (for this type and any wrapper like `PromiseFuture`).
    // After the first poll, `awaiter` holds an in-place-constructed C++ object that is (a) a
    // `kj::_::Event` registered with the event loop, (b) the target of the promise node's
    // self-pointer (`setSelfPointer` points INTO this memory), and (c) possibly threaded into a
    // `FuturePollEvent`'s intrusive `leaves` list. Moving `self` after that (e.g. `let g = f;`
    // after a `&mut f` partial await, which `Unpin` would permit in safe code) leaves all three
    // pointers dangling -- use-after-free when the promise fires. `PhantomPinned` turns that into
    // a compile error; ordinary `.await` pins structurally and is unaffected.
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
            // `node` is `Some` on this first (initializing) invocation; see the comment above.
            #[expect(
                clippy::expect_used,
                reason = "get_awaiter initializes exactly once while node is Some (awaiter_initialized guards re-entry); None here is an unreachable internal-invariant violation"
            )]
            let node = node.expect("node should be Some in call to init()");

            // Safety: The memory slot is valid and this type ensures that it will stay pinned.
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe {
                crate::ffi::guarded_rust_promise_awaiter_new_in_place(
                    this.awaiter
                        .as_mut_ptr()
                        .cast::<GuardedRustPromiseAwaiter>(),
                    node,
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
        // If the Waker driving this poll lends out a C++ PollWaker, take the optimized entry
        // point, which may arm the enclosing `co_await`'s KJ Event directly. Both borrows live
        // off `cx` for the duration of the call.
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
            // SAFETY: `awaiter_initialized` is true, so `self.awaiter` holds a
            // `GuardedRustPromiseAwaiter` constructed in place by `get_awaiter`; drop it in
            // place exactly once here (this is the only drop path, and `self` is being dropped).
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
/// `kj::Maybe<rust::Box<RustWaker>>` — ordinary RAII on both sides of the bridge: `rust::Box` is
/// move-only and its destructor runs this type's (automatic) drop glue, so ownership is enforced
/// by the type system rather than by convention.
///
/// This whole seam is safe Rust: nothing about the Waker is decomposed, transmuted, or
/// reassembled.
pub struct RustWaker(std::task::Waker);

impl RustWaker {
    /// `Waker::wake_by_ref` on the owned clone. (cxx methods cannot consume `self: Box<Self>`,
    /// so the caller wakes by reference and then drops the box — together equivalent to
    /// `Waker::wake`.)
    pub fn wake(&self) {
        self.0.wake_by_ref();
    }

    /// `Waker::will_wake`: would the owned clone wake the same task as the borrowed `waker`?
    /// Lets C++ skip a re-clone when re-polled with an equivalent Waker.
    pub fn will_wake(&self, waker: &WakerRef) -> bool {
        self.0.will_wake(waker.0)
    }
}

/// Clone the borrowed Waker into an owned, heap-allocated handle for C++ to keep.
#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx requires opaque Rust types to cross the bridge as Box"
)]
pub fn clone_waker(waker: &WakerRef) -> Box<RustWaker> {
    Box::new(RustWaker(waker.0.clone()))
}
