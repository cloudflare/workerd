#![allow(clippy::needless_lifetimes)]
#![allow(clippy::missing_errors_doc)]
#![allow(clippy::unused_async)]
#![allow(clippy::must_use_candidate)]
#![allow(clippy::cast_possible_truncation)]
#![allow(clippy::should_panic_without_expect)]
#![allow(clippy::missing_panics_doc)]

pub mod test_date;
mod test_futures;
mod test_maybe;
mod test_own;
mod test_refcount;

use kj_rs::KjOwn;
use test_futures::clear_stashed_wakers_on_background_thread;
use test_futures::drop_stashed_future;
use test_futures::get_side_effect_counter;
use test_futures::join_wake_storm;
use test_futures::new_await_fulfillable_promise_future_void;
use test_futures::new_await_pending_promise_future_void;
use test_futures::new_clone_storm_future_void;
use test_futures::new_cloned_kj_waker_context_future_void;
use test_futures::new_cross_thread_wake_future_void;
use test_futures::new_drop_cancellable_promise_without_polling;
use test_futures::new_error_handling_future_void_infallible;
use test_futures::new_errored_future_void;
use test_futures::new_future_awaiting_cancellable_promise;
use test_futures::new_generic_then_optimized_drops_clone_future_void;
use test_futures::new_kj_errored_future_void;
use test_futures::new_layered_ready_future_void;
use test_futures::new_multi_round_cross_thread_wake_future_void;
use test_futures::new_naive_select_future_void;
use test_futures::new_optimized_then_generic_future_void;
use test_futures::new_panic_on_drop_future_void;
use test_futures::new_panicking_after_await_future_void;
use test_futures::new_panicking_future_void;
use test_futures::new_panicking_infallible_future_void;
use test_futures::new_pending_future_void;
use test_futures::new_ping_pong_future_void;
use test_futures::new_promise_i32_awaiting_future_void;
use test_futures::new_ready_future_i32;
use test_futures::new_ready_future_void;
use test_futures::new_select_with_cancellation;
use test_futures::new_shared_stash_waker_future_void;
use test_futures::new_side_effect_future_void;
use test_futures::new_stash_then_ready_future_void;
use test_futures::new_stash_wakers_future_void;
use test_futures::new_sync_wake_storm_future_void;
use test_futures::new_threaded_delay_future_void;
use test_futures::new_two_step_cancellable_future;
use test_futures::new_wake_storm_future_void;
use test_futures::new_waker_reuse_and_replace_future_void;
use test_futures::new_waking_future_void;
use test_futures::new_wrapped_waker_future_void;
use test_futures::poll_and_stash_promise_future;
use test_futures::reset_ping_pong;
use test_futures::reset_side_effect_counter;
use test_futures::start_wake_storm;
use test_futures::unstash_and_await_promise_future;
use test_futures::wake_delayed_future;
use test_futures::wake_shared_stashed_waker;
use test_futures::wake_stashed_waker_from_background_thread;
use test_maybe::take_maybe_own;
use test_maybe::take_maybe_own_ret;
use test_maybe::take_maybe_ref;
use test_maybe::take_maybe_ref_ret;
use test_maybe::take_maybe_shared;
use test_maybe::take_maybe_shared_ret;
use test_refcount::modify_own_ret_arc;
use test_refcount::modify_own_ret_rc;
use test_refcount::take_maybe_rc_ret;

type Result<T> = std::io::Result<T>;
type Error = std::io::Error;

#[cxx::bridge(namespace = "kj_rs_demo")]
pub mod ffi {
    struct Shared {
        i: i64,
    }

    unsafe extern "C++" {
        include!("kj-rs-demo/test-promises.h");

        async fn new_ready_promise_void();
        async fn new_pending_promise_void();
        async fn new_coroutine_promise_void();

        async fn new_errored_promise_void();
        async fn new_ready_promise_i32(value: i32) -> i32;
        async fn new_ready_promise_shared_type() -> Shared;

        // Cancellation testing helpers.
        fn reset_cancellation_counter();
        fn get_cancellation_counter() -> u64;
        async fn new_cancellation_detecting_promise_void();

        // Manually fulfillable promise helpers.
        async fn new_fulfillable_promise_void();
        fn fulfill_stored_promise();
    }

    // Helper functions to test `kj_rs::KjOwn`
    unsafe extern "C++" {
        include!("kj-rs-demo/test-own.h");
        type OpaqueCxxClass;

        #[cxx_name = "getData"]
        fn get_data(&self) -> u64;
        #[cxx_name = "setData"]
        fn set_data(self: Pin<&mut OpaqueCxxClass>, val: u64);

        fn cxx_kj_own() -> KjOwn<OpaqueCxxClass>;
        fn null_kj_own() -> KjOwn<OpaqueCxxClass>;
        fn give_own_back(own: KjOwn<OpaqueCxxClass>);
        fn modify_own_return_test();
        fn breaking_things() -> KjOwn<OpaqueCxxClass>;

        fn own_integer() -> KjOwn<i64>;
        fn own_integer_attached() -> KjOwn<i64>;

        fn cxx_try_return_own() -> Result<KjOwn<OpaqueCxxClass>>;
        fn cxx_fail_return_own() -> Result<KjOwn<OpaqueCxxClass>>;

        fn null_exception_test_driver_1() -> String;
        fn null_exception_test_driver_2() -> String;
        fn rust_take_own_driver();
    }

    unsafe extern "C++" {
        include!("kj-rs-demo/test-refcount.h");

        type OpaqueRefcountedClass;

        fn get_rc() -> KjRc<OpaqueRefcountedClass>;
        #[cxx_name = "getData"]
        fn get_data(&self) -> u64;
        #[cxx_name = "setData"]
        fn set_data(self: Pin<&mut OpaqueRefcountedClass>, data: u64);

        fn give_rc_back(rc: KjRc<OpaqueRefcountedClass>);

        fn return_maybe_rc_some() -> KjMaybe<KjRc<OpaqueRefcountedClass>>;
        fn return_maybe_rc_none() -> KjMaybe<KjRc<OpaqueRefcountedClass>>;
        fn take_maybe_rc(maybe: KjMaybe<KjRc<OpaqueRefcountedClass>>);
        fn maybe_rc_rust_driver();
    }

    unsafe extern "C++" {
        include!("kj-rs-demo/test-refcount.h");

        type OpaqueAtomicRefcountedClass;

        fn get_arc() -> KjArc<OpaqueAtomicRefcountedClass>;

        #[cxx_name = "getData"]
        fn get_data(&self) -> u64;
        #[cxx_name = "setData"]
        fn set_data(self: Pin<&mut OpaqueAtomicRefcountedClass>, data: u64);

        fn give_arc_back(arc: KjArc<OpaqueAtomicRefcountedClass>);

        fn return_maybe_arc_some() -> KjMaybe<KjArc<OpaqueAtomicRefcountedClass>>;
        fn return_maybe_arc_none() -> KjMaybe<KjArc<OpaqueAtomicRefcountedClass>>;
    }

    extern "Rust" {
        fn modify_own_ret_rc(rc: KjRc<OpaqueRefcountedClass>) -> KjRc<OpaqueRefcountedClass>;
        fn modify_own_ret_arc(
            arc: KjArc<OpaqueAtomicRefcountedClass>,
        ) -> KjArc<OpaqueAtomicRefcountedClass>;

        // Receives a `Maybe<Rc>` from C++ and returns one back, mutating the
        // pointee when present. Exercises `kj::Maybe<kj::Rc>` as a Rust return type.
        fn take_maybe_rc_ret(
            maybe: KjMaybe<KjRc<OpaqueRefcountedClass>>,
        ) -> KjMaybe<KjRc<OpaqueRefcountedClass>>;
    }

    // Helper function to test moving `Own` to C++
    extern "Rust" {
        fn modify_own_return(cpp_own: KjOwn<OpaqueCxxClass>) -> KjOwn<OpaqueCxxClass>;
        fn take_own(cpp_own: KjOwn<OpaqueCxxClass>);
        fn get_null() -> KjOwn<OpaqueCxxClass>;
    }

    unsafe extern "C++" {
        include!("kj-rs-demo/test-maybe.h");

        fn test_maybe_reference_shared_own_driver();

        fn return_maybe() -> KjMaybe<i64>;
        fn return_maybe_none() -> KjMaybe<i64>;
        fn return_maybe_ref_some<'a>() -> KjMaybe<&'a i64>;
        fn return_maybe_ref_none<'a>() -> KjMaybe<&'a i64>;
        fn return_maybe_shared_some() -> KjMaybe<Shared>;
        fn return_maybe_shared_none() -> KjMaybe<Shared>;
        fn return_maybe_own_some() -> KjMaybe<KjOwn<OpaqueCxxClass>>;
        fn return_maybe_own_none() -> KjMaybe<KjOwn<OpaqueCxxClass>>;
        fn take_maybe_own_cxx(own: KjMaybe<KjOwn<OpaqueCxxClass>>);

        fn cxx_take_maybe_shared_some(maybe: KjMaybe<Shared>);
        fn cxx_take_maybe_shared_none(maybe: KjMaybe<Shared>);
        fn cxx_take_maybe_ref_shared_some(maybe: KjMaybe<&Shared>);
        fn cxx_take_maybe_ref_shared_none(maybe: KjMaybe<&Shared>);
    }

    unsafe extern "C++" {
        include!("kj-rs-demo/test-maybe.h");

        fn test_maybe_u8_some() -> KjMaybe<u8>;
        fn test_maybe_u16_some() -> KjMaybe<u16>;
        fn test_maybe_u32_some() -> KjMaybe<u32>;
        fn test_maybe_u64_some() -> KjMaybe<u64>;
        fn test_maybe_usize_some() -> KjMaybe<usize>;
        fn test_maybe_i8_some() -> KjMaybe<i8>;
        fn test_maybe_i16_some() -> KjMaybe<i16>;
        fn test_maybe_i32_some() -> KjMaybe<i32>;
        fn test_maybe_i64_some() -> KjMaybe<i64>;
        fn test_maybe_isize_some() -> KjMaybe<isize>;
        fn test_maybe_f32_some() -> KjMaybe<f32>;
        fn test_maybe_f64_some() -> KjMaybe<f64>;
        fn test_maybe_bool_some() -> KjMaybe<bool>;
        fn test_maybe_str_some() -> KjMaybe<&'static str>;
        fn test_maybe_u8_slice_some() -> KjMaybe<&'static [u8]>;
        fn test_maybe_pin_mut_some<'a>() -> KjMaybe<Pin<&'a mut u64>>;

        fn test_maybe_u8_none() -> KjMaybe<u8>;
        fn test_maybe_u16_none() -> KjMaybe<u16>;
        fn test_maybe_u32_none() -> KjMaybe<u32>;
        fn test_maybe_u64_none() -> KjMaybe<u64>;
        fn test_maybe_usize_none() -> KjMaybe<usize>;
        fn test_maybe_i8_none() -> KjMaybe<i8>;
        fn test_maybe_i16_none() -> KjMaybe<i16>;
        fn test_maybe_i32_none() -> KjMaybe<i32>;
        fn test_maybe_i64_none() -> KjMaybe<i64>;
        fn test_maybe_isize_none() -> KjMaybe<isize>;
        fn test_maybe_f32_none() -> KjMaybe<f32>;
        fn test_maybe_f64_none() -> KjMaybe<f64>;
        fn test_maybe_bool_none() -> KjMaybe<bool>;

        fn test_maybe_str_none() -> KjMaybe<&'static str>;
        fn test_maybe_u8_slice_none() -> KjMaybe<&'static [u8]>;
        fn test_maybe_pin_mut_none<'a>() -> KjMaybe<Pin<&'a mut u64>>;

        // `KjMaybe<String>` (owned string) round-trip: distinct from `KjMaybe<&str>` above, which
        // is only borrows. The `_empty` variant proves that `Some("")` survives the FFI without
        // being conflated with `None`.
        fn test_maybe_string_some() -> KjMaybe<String>;
        fn test_maybe_string_empty() -> KjMaybe<String>;
        fn test_maybe_string_none() -> KjMaybe<String>;

        // Round-trip: C++ verifies whether the Rust-supplied `KjMaybe<String>` was Some/empty/None.
        fn cxx_take_maybe_string_some(maybe: KjMaybe<String>);
        fn cxx_take_maybe_string_empty(maybe: KjMaybe<String>);
        fn cxx_take_maybe_string_none(maybe: KjMaybe<String>);
    }

    extern "Rust" {
        fn take_maybe_own_ret(
            val: KjMaybe<KjOwn<OpaqueCxxClass>>,
        ) -> KjMaybe<KjOwn<OpaqueCxxClass>>;
        fn take_maybe_own(val: KjMaybe<KjOwn<OpaqueCxxClass>>);
        unsafe fn take_maybe_ref_ret<'a>(val: KjMaybe<&'a u64>) -> KjMaybe<&'a u64>;
        fn take_maybe_ref(val: KjMaybe<&u64>);
        fn take_maybe_shared_ret(val: KjMaybe<Shared>) -> KjMaybe<Shared>;
        fn take_maybe_shared(val: KjMaybe<Shared>);
    }

    enum CloningAction {
        None,
        CloneSameThread,
        CloneBackgroundThread,
        WakeByRefThenCloneSameThread,
    }

    enum WakingAction {
        None,
        WakeByRefSameThread,
        WakeByRefBackgroundThread,
        WakeSameThread,
        WakeBackgroundThread,
    }

    // Helper functions to create BoxFutureVoids for testing purposes.
    extern "Rust" {
        async fn new_pending_future_void();
        async fn new_ready_future_void();
        async fn new_ready_future_shared_type() -> Shared;
        async fn new_waking_future_void(cloning_action: CloningAction, waking_action: WakingAction);
        async fn new_threaded_delay_future_void();
        // Woken from a spawned foreign thread (no KJ event loop): exercises the cross-thread
        // `Waker` contract end to end.
        async fn new_cross_thread_wake_future_void();
        // Foreign-thread wakes across multiple polls: exercises the cross-thread fulfiller's
        // per-poll renewal.
        async fn new_multi_round_cross_thread_wake_future_void();
        // Takes the waker stashed by `new_threaded_delay_future_void` and wakes it from a joined
        // foreign thread — used both while the future is alive and after it has been destroyed
        // (the neutralized cross-thread late wake).
        fn wake_stashed_waker_from_background_thread();
        // Wakes the waker stashed by `new_threaded_delay_future_void`'s future, on the loop thread,
        // to drive an asynchronous same-thread wake after poll() has returned.
        fn wake_delayed_future();

        // Multithreaded stress helpers (TSAN targets; see test_futures.rs for the full story).
        //
        // Concurrent foreign-thread wake storm against a live, re-polling future.
        async fn new_wake_storm_future_void(threads: u32, wakes_per_thread: u32);
        // Concurrent clone/drop refcount churn from foreign threads.
        async fn new_clone_storm_future_void(threads: u32, clones_per_thread: u32);
        // Stash waker clones globally and stay Pending forever; the driver storms and cancels.
        async fn new_stash_wakers_future_void(clones: u32);
        // Spawn threads waking every stashed waker in a loop; runs concurrently with the driver.
        fn start_wake_storm(threads: u32, iterations: u32);
        // Join the storm threads.
        fn join_wake_storm();
        // Drop the stashed clones on a joined foreign thread (after the future's death).
        fn clear_stashed_wakers_on_background_thread();
        // Shared-slot waker stash + wake-from-anywhere: another loop's thread, or after loop death.
        async fn new_shared_stash_waker_future_void();
        fn wake_shared_stashed_waker();

        // Coverage-gap helpers (see test_futures.rs).
        //
        // N same-thread wake_by_ref() calls inside one poll (armDepthFirst idempotence).
        async fn new_sync_wake_storm_future_void(wakes: u32);
        // Stashes a waker then completes immediately: waking it later hits isDone()'s early return.
        async fn new_stash_then_ready_future_void();
        // Parked on a never-resolving KJ promise, for kj::Promise::trace() through the bridge.
        async fn new_await_pending_promise_future_void() -> Result<()>;
        /// Awaits the fulfillable promise (`new_fulfillable_promise_void`); re-polled when the
        /// C++ side calls `fulfill_stored_promise()`.
        async fn new_await_fulfillable_promise_future_void() -> Result<()>;
        // RustPromiseAwaiter path transitions: optimized -> generic (fire wakes the stored custom
        // waker) and generic -> optimized (the stored clone is dropped).
        async fn new_optimized_then_generic_future_void();
        async fn new_generic_then_optimized_drops_clone_future_void() -> Result<()>;
        // Generic path storing a clone of a KJ cell waker (a Context built from a cloned waker).
        async fn new_cloned_kj_waker_context_future_void();
        // Two loops on two threads waking each other for N rounds.
        fn reset_ping_pong();
        async fn new_ping_pong_future_void(side: u32, rounds: u32);

        async fn new_layered_ready_future_void() -> Result<()>;

        async fn new_naive_select_future_void() -> Result<()>;
        async fn new_wrapped_waker_future_void() -> Result<()>;

        async fn new_errored_future_void() -> Result<()>;

        // Unwind protection (kj-rs/future.rs): panics escaping a bridged future's poll()
        // must become rejected promises (kj::Exception), not process aborts.
        async fn new_panicking_future_void() -> Result<()>;
        /// Never ready; its `Drop` panics. Dropping the promise must abort (death test), never
        /// unwind into C++.
        async fn new_panic_on_drop_future_void();
        async fn new_panicking_infallible_future_void();
        async fn new_panicking_after_await_future_void() -> Result<()>;

        async fn new_kj_errored_future_void() -> Result<()>;

        async fn new_error_handling_future_void_infallible();

        async fn new_promise_i32_awaiting_future_void() -> Result<()>;
        async fn new_ready_future_i32(value: i32) -> Result<i32>;
        async fn new_pass_through_feature_shared() -> Shared;

        // Eager-by-default test helpers. The bridge's conversion polls the future
        // synchronously to its first suspension at promise creation. The cold-promise
        // (`RustFuture::lazily()`) counterparts bypass the bridge: they hand C++ the raw
        // `RustFuture` through plain `extern "C"` helpers (see `test_futures.rs`).
        #[expect(clippy::allow_attributes)] // Only called from C++ tests; #[expect(dead_code)] fails in builds where the lint does not fire
        #[allow(dead_code)]
        fn reset_side_effect_counter();
        #[expect(clippy::allow_attributes)] // Only called from C++ tests; #[expect(dead_code)] fails in builds where the lint does not fire
        #[allow(dead_code)]
        fn get_side_effect_counter() -> u64;
        async fn new_side_effect_future_void();

        // Cancellation test helpers.
        async fn new_future_awaiting_cancellable_promise() -> Result<()>;
        async fn new_two_step_cancellable_future() -> Result<()>;
        async fn new_select_with_cancellation() -> Result<()>;
        async fn new_drop_cancellable_promise_without_polling() -> Result<()>;

        // NaughtyFuture test helpers.
        async fn poll_and_stash_promise_future() -> Result<()>;
        async fn unstash_and_await_promise_future() -> Result<()>;
        // Drop the stashed future without awaiting it — called after the event loop is
        // destroyed, exercising the teardown-tolerant drop path.
        fn drop_stashed_future();

        // Stored-Waker semantics: exact will_wake / replace / drop behavior of the generic
        // (non-KJ-waker) poll path, asserted via Arc strong counts.
        async fn new_waker_reuse_and_replace_future_void() -> Result<()>;
    }

    // these are used to check compilation only
    extern "Rust" {

        async unsafe fn lifetime_arg_void<'a>(buf: &'a [u8]);
        async unsafe fn lifetime_arg_result<'a>(buf: &'a [u8]) -> Result<()>;
    }

    struct StructWithMaybe {
        b: bool,
        u: KjMaybe<u64>,
    }

    extern "Rust" {
        async unsafe fn pass_struct_with_maybe(x: StructWithMaybe) -> Result<()>;
    }
}

async fn pass_struct_with_maybe(_x: ffi::StructWithMaybe) -> Result<()> {
    Ok(())
}

// Safety: the test type follows the thread-safety contract of its C++ implementation.
unsafe impl Send for ffi::OpaqueAtomicRefcountedClass {}
// Safety: the test type follows the thread-safety contract of its C++ implementation.
unsafe impl Sync for ffi::OpaqueAtomicRefcountedClass {}

// Compile-time thread-safety contracts (kj-rs/own.rs, kj-rs/refcount.rs):
//
// KjOwn is never Send/Sync: its type-erased kj disposer (kj::Rc, arena, ...) may not be
// thread-safe, regardless of T.
static_assertions::assert_not_impl_any!(KjOwn<u64>: Send, Sync);
static_assertions::assert_not_impl_any!(KjOwn<ffi::OpaqueCxxClass>: Send, Sync);
// KjArc matches std::sync::Arc: Send/Sync require T: Send + Sync...
static_assertions::assert_impl_all!(kj_rs::KjArc<ffi::OpaqueAtomicRefcountedClass>: Send, Sync);
// ...so a Send + !Sync payload (Cell) must make KjArc neither Send nor Sync (clones would
// otherwise hand concurrent &T to multiple threads).
static_assertions::assert_not_impl_any!(kj_rs::KjArc<std::cell::Cell<u64>>: Send, Sync);
// KjRc (non-atomic refcount) must never be Send or Sync.
static_assertions::assert_not_impl_any!(kj_rs::KjRc<ffi::OpaqueRefcountedClass>: Send, Sync);

pub fn modify_own_return(mut own: KjOwn<ffi::OpaqueCxxClass>) -> KjOwn<ffi::OpaqueCxxClass> {
    own.pin_mut().set_data(72);
    own
}

pub fn get_null() -> KjOwn<ffi::OpaqueCxxClass> {
    ffi::null_kj_own()
}

pub fn take_own(cpp_own: KjOwn<ffi::OpaqueCxxClass>) {
    assert_eq!(cpp_own.get_data(), 14);
    // The point of this function is to drop the [`Own`] from rust and this makes
    // it explicit, while avoiding a clippy lint
    std::mem::drop(cpp_own);
}

pub async fn lifetime_arg_void<'a>(_buf: &'a [u8]) {}

pub async fn lifetime_arg_result<'a>(_buf: &'a [u8]) -> Result<()> {
    Ok(())
}

/// # Panics
/// - if c++ side throws exception
pub async fn new_pass_through_feature_shared() -> ffi::Shared {
    match ffi::new_ready_promise_shared_type().await {
        Ok(value) => value,
        Err(error) => panic!("C++ promise failed: {error}"),
    }
}

async fn new_ready_future_shared_type() -> ffi::Shared {
    ffi::Shared { i: 42 }
}

fn work_before_poll(target: &mut u64) -> impl Future<Output = Result<()>> {
    *target = 42;

    async move {
        unimplemented!("not expected to be polled");
    }
}

/// Hands C++ the raw, not-yet-converted future from [`work_before_poll`].
///
/// The returned future must never be polled (its body panics), so it cannot go through a
/// bridged `async fn` shim: those always apply `RustFuture`'s eager-by-default
/// `kj::Promise` conversion, which polls at creation. The C++ test converts it with
/// `RustFuture::lazily()` instead (see `awaitables-cc-test.c++`).
///
/// # Safety
///
/// `target` must be a valid, exclusive `u64` pointer that outlives the future; `out` must
/// point to uninitialized storage for one `::kj_rs::repr::RustFuture` (future.h), which the
/// caller takes ownership of.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kj_rs_demo_work_before_poll<'a>(
    target: &'a mut u64,
    out: *mut kj_rs::repr::RustFuture<'a, ()>,
) {
    let fut = kj_rs::repr::future(Box::pin(kj_rs::map_err(
        work_before_poll(target),
        file!(),
        line!(),
    )));
    // SAFETY: `out` points to uninitialized storage for one RustFuture, per this fn's
    // `# Safety` contract.
    unsafe { out.write(fut) };
}

#[cfg(test)]
mod tests {
    use crate::ffi;

    #[test]
    fn compilation() {
        // These promises can't be driven by the Rust side, so just check that they compile.
        drop(ffi::new_ready_promise_void());
        drop(ffi::new_ready_promise_i32(42));
        drop(ffi::new_ready_promise_shared_type());
    }
}
