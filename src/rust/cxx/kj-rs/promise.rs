#![allow(
    unsafe_code,
    reason = "owns C++ promise nodes and pin-projects the awaiter that drives them"
)]

use std::cell::Cell;
use std::ffi::c_void;
use std::future::Future;
use std::marker::PhantomData;
use std::mem::ManuallyDrop;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use cxx::ExternType;
use cxx::core::mem::MaybeUninit;

use crate::PromiseAwaiter;
use crate::loop_checked::EventLoopId;

type CxxResult<T> = std::result::Result<T, cxx::KjException>;

#[repr(transparent)]
pub struct OwnPromiseNode(*mut c_void /* kj::_::PromiseNode* */);

// Note: drop is not the only way for OwnPromiseNode to be destroyed.
// It is forgotten using `MaybeUninit` and its ownership passed over to c++ in `unwrap`.
impl Drop for OwnPromiseNode {
    fn drop(&mut self) {
        // Safety:
        // 1. Pointer to self is non-null, and obviously points to valid memory.
        // 2. We do not read or write to the OwnPromiseNode's memory, so there are no atomicity nor
        //    interleaved pointer/reference access concerns.
        //
        // https://doc.rust-lang.org/std/ptr/index.html#safety
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
        unsafe {
            crate::ffi::own_promise_node_drop_in_place(self);
        }
    }
}

// Safety: We have a static_assert in promise.c++ which breaks if you change the size or alignment
// of the C++ definition of OwnPromiseNode, with a comment directing the reader to adjust the
// OwnPromiseNode definition in this .rs file.
//
// https://docs.rs/cxx/latest/cxx/trait.ExternType.html#integrating-with-bindgen-generated-types
// Safety: the KJ bridge representation and ownership invariants satisfy this operation.
unsafe impl ExternType for OwnPromiseNode {
    type Id = cxx::type_id!("::kj_rs::OwnPromiseNode");
    type Kind = cxx::kind::Trivial;
}

pub trait KjPromise: Sized {
    type Output;
    type Data;
    fn into_own_promise_node(self) -> OwnPromiseNode;

    /// # Errors
    ///
    /// Returns an error when C++ side generated an exception.
    ///
    /// # Safety
    ///
    /// You must guarantee that `node` was previously returned from this same type's
    /// `into_own_promise_node()` implementation.
    /// node is supposed to be already resolved
    unsafe fn unwrap(node: OwnPromiseNode, data: &Self::Data) -> CxxResult<Self::Output>;
}

/// A future of a KJ promise, which belongs to the event loop that made the promise.
pub struct PromiseFuture<P: KjPromise> {
    awaiter: PromiseAwaiter<P::Data>,
    _marker: PhantomData<P>,
}

impl<P: KjPromise> PromiseFuture<P> {
    /// `loop_id` is the id of the event loop `promise` belongs to.
    pub(crate) fn new(promise: P, data: P::Data, loop_id: EventLoopId) -> Self {
        Self {
            awaiter: PromiseAwaiter::new(promise.into_own_promise_node(), data, loop_id),
            _marker: PhantomData,
        }
    }

    /// Polls the promise, first checking that its event loop is current if `checked`.
    fn poll_promise(
        self: Pin<&mut Self>,
        cx: &mut Context,
        checked: bool,
    ) -> Poll<CxxResult<P::Output>> {
        // Safety: `awaiter` is structurally pinned within `PromiseFuture` -- it is never moved
        // after pinning, and `PromiseFuture` has no `Drop` impl that could move it.
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
        let mut awaiter = unsafe { self.map_unchecked_mut(|s| &mut s.awaiter) };
        let ready = if checked {
            match awaiter.as_mut().poll_checked(cx) {
                Ok(ready) => ready,
                Err(err) => return Poll::Ready(Err(err)),
            }
        } else {
            awaiter.as_mut().poll(cx)
        };
        if ready {
            let node = awaiter.as_mut().take_own_promise_node();
            // Safety: `node` was created by `P::into_own_promise_node()` in `PromiseFuture::new()`,
            // and the promise is resolved (poll returned true).
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let value = unsafe { P::unwrap(node, &awaiter.data) };
            Poll::Ready(value)
        } else {
            Poll::Pending
        }
    }
}

impl<P: KjPromise> Future for PromiseFuture<P> {
    type Output = CxxResult<P::Output>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        self.poll_promise(cx, false)
    }
}

mod sealed {
    use std::pin::Pin;
    use std::task::Context;
    use std::task::Poll;

    pub trait Sealed: Future {
        /// Whether the future's event loop is the current thread's.
        fn is_current(&self) -> bool;

        /// Polls the future, or, off its event loop, returns an error without touching it.
        fn poll_checked(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output>;
    }
}

/// The future of the KJ promise that a C++ `async` function returns. It belongs to the event loop
/// that made the promise, so it is not `Send`; [`into_send()`](Self::into_send) makes it so.
pub trait KjFuture: Future + Sized + sealed::Sealed {
    /// Makes the future `Send`, so that a `Send` future can hold it while its event loop drives
    /// both, as `IoContext::await_io` does.
    fn into_send(self) -> SendKjFuture<Self> {
        SendKjFuture {
            inner: ManuallyDrop::new(self),
            _not_sync: PhantomData,
        }
    }
}

impl<T> sealed::Sealed for PromiseFuture<CallbacksFuture<T>> {
    fn is_current(&self) -> bool {
        self.awaiter.is_current()
    }

    fn poll_checked(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        self.poll_promise(cx, true)
    }
}

impl<T> KjFuture for PromiseFuture<CallbacksFuture<T>> {}

/// A [`KjFuture`] that is `Send`, and may move between OS threads with its event loop.
///
/// Polling it off its event loop returns an error without touching the promise. Dropping it off its
/// loop aborts the process, even once the loop is gone, as destroying its promise there would be
/// unsound, so after such an error the caller must keep the future until it is back on its loop,
/// rather than dropping it as `?` and combinators such as `select!` do. It is not `Sync`.
pub struct SendKjFuture<F: KjFuture> {
    inner: ManuallyDrop<F>,
    _not_sync: PhantomData<Cell<()>>,
}

// SAFETY: `inner` is a bridged `PromiseFuture` (`KjFuture` is sealed), which holds only its KJ
// promise, the id of the promise's event loop, and a waker, and is polled and dropped only while
// that event loop is current. A KJ promise depends on its event loop, not on the OS thread
// that runs it, and an embedder may run an event loop, with its promises, on different OS threads
// between turns; the arguments the C++ function borrowed, which the promise may use, are its
// caller's, on the same loop. Not being `Sync` keeps a `&F` from reaching another thread.
unsafe impl<F: KjFuture> Send for SendKjFuture<F> {}

impl<F: KjFuture> Future for SendKjFuture<F> {
    type Output = F::Output;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<F::Output> {
        // SAFETY: `inner` is structurally pinned: nothing moves it out of `self`, and `drop()`
        // drops it in place.
        let inner = unsafe { self.map_unchecked_mut(|this| &mut *this.inner) };
        inner.poll_checked(cx)
    }
}

impl<F: KjFuture> Drop for SendKjFuture<F> {
    fn drop(&mut self) {
        if !self.inner.is_current() {
            crate::ffi::loop_checked_dropped_off_loop();
            std::process::abort();
        }
        // SAFETY: `inner` is not used again, and is dropped in place, on its event loop.
        unsafe { ManuallyDrop::drop(&mut self.inner) }
    }
}

type UnwrapCallback =
    unsafe extern "C" fn(node: *mut c_void, ret: *mut c_void) -> cxx::private::Result;

#[derive(Copy, Clone)]
#[repr(C)]
pub struct KjPromiseNodeImpl {
    pub node: *mut c_void,
    pub unwrap: UnwrapCallback,
    /// The id of the event loop `node` belongs to.
    pub loop_id: EventLoopId,
}

/// Makes a future of the `kj::Promise<T>` that `impl` describes. Generated bridge code calls this
/// with the value an `async` C++ function returned.
///
/// # Safety
///
/// `impl.node` must point to a `kj::_::PromiseNode` that nothing else owns, since the future takes
/// ownership of it, and `impl.loop_id` must be the id of the event loop that node belongs to.
/// `impl.unwrap` must be the callback C++ made for that node, whose result is a `T`.
pub unsafe fn new_callbacks_promise_future<T>(
    r#impl: &KjPromiseNodeImpl,
) -> impl KjFuture<Output = CxxResult<T>> + use<T> {
    PromiseFuture::new(
        CallbacksFuture {
            node: r#impl.node,
            _phantom: PhantomData,
        },
        FutureCallbacks {
            unwrap: r#impl.unwrap,
        },
        r#impl.loop_id,
    )
}

pub struct FutureCallbacks {
    pub unwrap: UnwrapCallback,
}

pub struct CallbacksFuture<T> {
    pub node: *mut c_void,
    _phantom: PhantomData<T>,
}

impl<T> KjPromise for CallbacksFuture<T> {
    type Output = T;
    type Data = FutureCallbacks;

    fn into_own_promise_node(self) -> OwnPromiseNode {
        OwnPromiseNode(self.node)
    }

    unsafe fn unwrap(node: OwnPromiseNode, callbacks: &FutureCallbacks) -> CxxResult<Self::Output> {
        let mut ret = MaybeUninit::<Self::Output>::uninit();
        // unwrap will take over node ownership
        let node = ManuallyDrop::new(node);

        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
        unsafe { (callbacks.unwrap)(node.0, ret.as_mut_ptr().cast::<c_void>()).into_result() }?;
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
        Ok(unsafe { ret.assume_init() })
    }
}

// Safety: the KJ bridge representation and ownership invariants satisfy this operation.
unsafe impl<T: Send> Send for CallbacksFuture<T> {}

#[cfg(test)]
#[path = "promise-test.rs"]
mod tests;
