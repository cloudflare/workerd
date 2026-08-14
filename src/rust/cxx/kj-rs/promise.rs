//! FFI island (see crate-root `#![deny(unsafe_code)]`): `OwnPromiseNode`/`PromiseFuture` bridge —
//! `unsafe impl ExternType`, `unsafe extern "C"` unwrap callbacks, and `Pin` projection. A genuine
//! unsafe seam.
//!
//! # Object-relationship overview (both bridge directions)
//!
//! C++ `co_await`s a Rust Future (awaiter.h, waker.h, future.rs):
//!
//! ```text
//! FutureAwaiter<F> — a FuturePollEvent: the kj Event whose fire() polls the Future
//!     │
//!     ├── owns one persistent FutureWakerCell (kj::Arc — atomically refcounted, thread-safe)
//!     │       ├── weak Event link — nulled by the event's destructor, so wakes retained by
//!     │       │     Rust past the future's life are safe no-ops (neutralize-on-drop)
//!     │       ├── owning Executor — routes wakes: on the owning thread, arm the event
//!     │       │     directly; from any other thread, enqueue on the loop's sink
//!     │       └── Arc<CrossThreadWakeSink> — ONE per event loop (kj::EventLoopLocal): a
//!     │             mutexed queue of cells + a single cross-thread fulfiller whose drain
//!     │             coroutine replays queued wakes on the owning thread; closed with the loop
//!     │
//!     ├── each poll creates a stack PollWaker (lent to Rust as the poll's `&Waker`)
//!     │       ├── wake_by_ref — delegates to the cell (same-turn re-poll when same-thread)
//!     │       └── clone — hands out a new Arc ref to the cell (the retained-waker path)
//!     │
//!     └── leaves — intrusive list of the RustPromiseAwaiters (below) this Future is
//!           currently `.await`ing; any of them becoming ready arms this one shared event
//! ```
//!
//! Rust `.await`s a KJ Promise (this file, awaiter.rs, awaiter.h):
//!
//! ```text
//! OwnPromiseNode — owned raw kj::_::PromiseNode*; !Send, so single-loop by construction
//!     │  IntoFuture
//!     ▼
//! PromiseFuture — owns a GuardedRustPromiseAwaiter in place (executor-guarded C++ object)
//!     │
//!     ▼
//! RustPromiseAwaiter — kj Event registered via node->onReady(); its fire() = promise ready
//!     ├── kj::Weak<FuturePollEvent> + link — weak edge into a FuturePollEvent's `leaves`:
//!     │     the optimized path when polled under a C++ co_await (readiness arms the event
//!     │     directly, no Waker involved); the weak expires on the event's destruction and
//!     │     the intrusive list is unlinked by either side's destructor
//!     └── storedWaker — OUR OWN clone of an arbitrary `Waker`, held as its raw words
//!           (RawWakerParts): the generic path when polled by any other runtime; fire()
//!           reassembles and wakes it instead
//! ```
#![allow(unsafe_code)]

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

type CxxResult<T> = std::result::Result<T, cxx::KjException>;

#[repr(transparent)]
pub struct OwnPromiseNode(*mut c_void /* kj::_::PromiseNode* */);

// Note: drop is not the only way for OwnPromiseNode to be destroyed.
// It is forgotten using `MaybeUninit` and its ownership passed over to c++ in `unwrap`.
impl Drop for OwnPromiseNode {
    fn drop(&mut self) {
        // `own_promise_node_drop_in_place` placement-destructs the node behind `self`. The
        // borrow is valid for the call; the value is only logically dead afterwards, inside
        // this `drop`, and the inner `*mut c_void` has no drop glue, so there is no
        // use-after-free or double-free. Expressed as a `&mut` binding, so no `unsafe` needed.
        crate::ffi::own_promise_node_drop_in_place(self);
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
    type Data: std::marker::Unpin;
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

pub struct PromiseFuture<P: KjPromise> {
    awaiter: PromiseAwaiter<P::Data>,
    _marker: PhantomData<P>,
}

impl<P: KjPromise> PromiseFuture<P> {
    pub fn new(promise: P, data: P::Data) -> Self {
        Self {
            awaiter: PromiseAwaiter::new(promise.into_own_promise_node(), data),
            _marker: PhantomData,
        }
    }
}

impl<P: KjPromise> Future for PromiseFuture<P> {
    type Output = CxxResult<P::Output>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        // Safety: `awaiter` is structurally pinned within `PromiseFuture` -- it is never moved
        // after pinning, and `PromiseFuture` has no `Drop` impl that could move it.
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
        let mut awaiter = unsafe { self.map_unchecked_mut(|s| &mut s.awaiter) };
        if awaiter.as_mut().poll(cx) {
            let node = awaiter.as_mut().get_awaiter().take_own_promise_node();
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

type UnwrapCallback =
    unsafe extern "C" fn(node: *mut c_void, ret: *mut c_void) -> cxx::private::Result;

#[derive(Copy, Clone)]
#[repr(C)]
pub struct KjPromiseNodeImpl {
    pub node: *mut c_void,
    pub unwrap: UnwrapCallback,
}

pub fn new_callbacks_promise_future<T>(
    r#impl: &KjPromiseNodeImpl,
) -> impl Future<Output = CxxResult<T>> + use<T> {
    PromiseFuture::new(
        CallbacksFuture {
            node: r#impl.node,
            _phantom: PhantomData,
        },
        FutureCallbacks {
            unwrap: r#impl.unwrap,
        },
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

        // SAFETY: `node.0` is a live `OwnPromiseNode` whose ownership the callback takes over
        // (wrapped in `ManuallyDrop` so we don't also drop it); `ret` is valid, suitably-aligned
        // uninitialized storage for `Output`, which the callback initializes on the success path.
        unsafe { (callbacks.unwrap)(node.0, ret.as_mut_ptr().cast::<c_void>()).into_result() }?;
        // SAFETY: the `?` above propagated any error, so on this path the callback reported
        // success and therefore initialized `ret`.
        Ok(unsafe { ret.assume_init() })
    }
}

// No `unsafe impl Send for CallbacksFuture<T>`, deliberately.
//
// `CallbacksFuture` is only ever wrapped in `PromiseFuture<Self>`, whose `PromiseAwaiter` holds an
// `Option<OwnPromiseNode>` (a raw pointer, hence `!Send`), so the composed future is `!Send`
// regardless. The bridged async machinery is confined to the KJ event-loop thread and `spawn` is
// `spawn_local`-backed (no `Send` requirement), so nothing needs a `Send` impl. Asserting the
// wrapper stays `!Send` locks that in.
#[cfg(test)]
mod send_guards {
    use static_assertions::assert_not_impl_any;

    use super::CallbacksFuture;
    use super::PromiseFuture;

    // The raw `*mut c_void` node makes this `!Send`/`!Sync` on its own; guard against a future
    // hand-written impl silently introducing cross-thread transfer of a KJ promise node.
    assert_not_impl_any!(CallbacksFuture<u32>: Send, Sync);

    // After its first poll, `PromiseFuture`'s embedded awaiter memory is self-referential and
    // event-loop-linked (see `PromiseAwaiter::_pinned`); it must stay `!Unpin` so safe code
    // cannot move it between polls (`&mut`-based awaits require `Unpin`).
    assert_not_impl_any!(PromiseFuture<CallbacksFuture<u32>>: Unpin);
}
