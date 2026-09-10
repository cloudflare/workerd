use std::mem::MaybeUninit;
use std::pin::Pin;
use std::task::Context;

use crate::OwnPromiseNode;
// =======================================================================================
// Await syntax for OwnPromiseNode
use crate::ffi::GuardedRustPromiseAwaiter;
use crate::ffi::GuardedRustPromiseAwaiterRepr;
use crate::waker::try_into_kj_waker_ptr;

pub struct PromiseAwaiter<Data: std::marker::Unpin> {
    node: Option<OwnPromiseNode>,
    pub(crate) data: Data,
    awaiter: MaybeUninit<GuardedRustPromiseAwaiterRepr>,
    awaiter_initialized: bool,
    // Safety: `option_waker` must be declared after `awaiter`, because `awaiter` contains a reference
    // to `option_waker`. This ensures `option_waker` will be dropped after `awaiter`.
    option_waker: OptionWaker,
}

impl<Data: std::marker::Unpin> PromiseAwaiter<Data> {
    pub fn new(node: OwnPromiseNode, data: Data) -> Self {
        Self {
            node: Some(node),
            data,
            awaiter: MaybeUninit::uninit(),
            awaiter_initialized: false,
            option_waker: OptionWaker::empty(),
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

            // Safety: `awaiter` stores `rust_waker_ptr` and uses it to call `wake()`. Note that
            // `awaiter` is `this.awaiter`, which lives before `this.option_waker`.
            // Since we drop awaiter manually, the `rust_waker_ptr` that `awaiter` stores will always
            // be valid during its lifetime.
            //
            // We pass a mutable pointer to C++. This is safe, because our use of the OptionWaker inside
            // of `std::task::Waker` is synchronized by ensuring we only allow calls to `poll()` on the
            // thread with the Promise's event loop active.
            let rust_waker_ptr = &raw mut this.option_waker;

            // Safety: The memory slot is valid and this type ensures that it will stay pinned.
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe {
                crate::ffi::guarded_rust_promise_awaiter_new_in_place(
                    this.awaiter
                        .as_mut_ptr()
                        .cast::<GuardedRustPromiseAwaiter>(),
                    rust_waker_ptr,
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
        let maybe_kj_waker = try_into_kj_waker_ptr(cx.waker());
        let awaiter = self.as_mut().get_awaiter();
        // Safety: The awaiter is initialized by `get_awaiter()` above. `WakerRef` borrows the
        // context's waker, which is alive for the duration of the call. `maybe_kj_waker` is null
        // or points to the KjWaker inside the waker (validated by `try_into_kj_waker_ptr`).
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
        unsafe { awaiter.poll(&WakerRef(cx.waker()), maybe_kj_waker) }
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
// OptionWaker and WakerRef

pub struct WakerRef<'a>(&'a std::task::Waker);

// This is a wrapper around `std::task::Waker`, exposed to C++. We use it in `RustPromiseAwaiter`
// to allow KJ promises to be awaited using opaque Wakers implemented in Rust.
pub struct OptionWaker {
    inner: Option<std::task::Waker>,
}

impl OptionWaker {
    pub fn empty() -> Self {
        Self { inner: None }
    }

    pub fn set(&mut self, waker: &WakerRef) {
        if let Some(w) = &mut self.inner {
            w.clone_from(waker.0);
        } else {
            self.inner = Some(waker.0.clone());
        }
    }

    pub fn set_none(&mut self) {
        self.inner = None;
    }

    /// Wake the stored Waker, if any. Does nothing if the inner Waker is None.
    ///
    /// The `OptionWaker` may be empty when `RustPromiseAwaiter::fire()` runs after `poll()` took the
    /// optimized path (which clears the `OptionWaker` and links to a `FuturePollEvent` instead) but
    /// the `FuturePollEvent` was destroyed before the promise fired. In that case there is nothing to
    /// wake; the owner's next `poll()` will discover the promise is ready.
    pub fn wake_if_some(&mut self) {
        if let Some(waker) = self.inner.take() {
            waker.wake();
        }
    }
}
