//! FFI island: the `RustFuture` C-ABI vtable that drives
//! all bridged async — `unsafe extern "C"` poll/drop callbacks, raw-pointer result writes, and
//! `Pin`/`Box` raw conversions. A genuine unsafe seam.

// This file contains boilerplate which must occur once per crate, rather than once per type.

#![allow(
    unsafe_code,
    reason = "C++ polls and drops type-erased futures through raw callbacks"
)]

use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use cxx::IntoKjException;

// NOTE: FuturePollStatus must be kept in sync with the C++ enum of the same name in future.h
// Ideally, this would live in kj-rs's `crate::ffi` module, and code which depends on kj-rs would be
// able to include `kj-rs/src/lib.rs.h`. I couldn't figure out how to expose that generated lib.rs.h
// header to Bazel dependents, though, so I'm just splatting it here.
#[derive(Copy, Clone, PartialEq, Eq)]
#[repr(transparent)]
pub struct FuturePollStatus {
    pub repr: u8,
}

impl FuturePollStatus {
    pub const PENDING: Self = Self { repr: 0 };
    pub const COMPLETE: Self = Self { repr: 1 };
    pub const ERROR: Self = Self { repr: 2 };
}

// These types are shared with C++ code.
pub mod repr {
    use std::ffi::c_void;
    use std::panic::AssertUnwindSafe;
    use std::pin::Pin;
    use std::task::Context;
    use std::task::Poll;

    use static_assertions::assert_eq_size;

    use super::FuturePollStatus;
    use crate::ffi::PollWaker;

    type PollCallback = for<'a> unsafe extern "C" fn(
        fut: *mut c_void,
        waker: &'a PollWaker,
        ret: *mut c_void,
    ) -> FuturePollStatus;

    type DropCallback = unsafe extern "C" fn(fut: *mut c_void);

    type FuturePtr<'a, T> = *mut (dyn Future<Output = Result<T, cxx::KjException>> + 'a);

    /// Represents a `dyn Future<Output = Result<T, cxx::KjException>>`.
    #[repr(C)]
    pub struct RustFuture<'a, T> {
        pub fut: FuturePtr<'a, T>,
        pub poll: PollCallback,
        pub drop: DropCallback,
    }

    type InfallibleFuturePtr<'a, T> = *mut (dyn Future<Output = T> + 'a);

    /// Represents a `dyn Future<Output = T>` where T is not a Result.
    #[repr(C)]
    pub struct RustInfallibleFuture<'a, T> {
        pub fut: InfallibleFuturePtr<'a, T>,
        pub poll: PollCallback,
        pub drop: DropCallback,
    }

    // `RustFuture<T>` and `RustInfallibleFuture<T>` have the same layout.
    // They exist separately because of rust trait type system limitations.
    assert_eq_size!(RustFuture<()>, RustInfallibleFuture<()>);

    assert_eq_size!(RustFuture<()>, [*mut c_void; 4]);
    assert_eq_size!(RustInfallibleFuture<()>, [*mut c_void; 4]);

    impl<T: Unpin> RustFuture<'_, T> {
        /// # Safety
        ///
        /// C++ `RustFuture` vtable protocol (future.h): `fut` must be the `fut` field of a
        /// live, not-yet-dropped `RustFuture<T>` created by [`future`]; `ret` must point to
        /// storage suitable for a `T`
        /// (Complete) or a `kj::Exception*` (Error), per `FuturePoller<T>`'s union.
        ///
        /// Unwind safety: any panic escaping the wrapped future's `poll` (or the waker
        /// machinery) is caught here and converted into an errored completion, because
        /// unwinding out of an `extern "C"` fn is instant process abort (Rust >= 1.81).
        /// This makes a panicking bridged `async fn` surface to C++ as a rejected
        /// `kj::Promise` carrying a `kj::Exception`, matching the sync bridge path.
        pub(crate) unsafe extern "C" fn poll(
            fut: *mut c_void,
            waker: &PollWaker,
            ret: *mut c_void,
        ) -> FuturePollStatus {
            // SAFETY: per this fn's `# Safety` contract, `fut` is the `fut` field of a live
            // `RustFuture<T>`, i.e. a valid `*mut FuturePtr<T>` we may read and then pin.
            let fut = unsafe { *(fut.cast::<FuturePtr<T>>()) };
            // SAFETY: the boxed future is never moved out of its heap allocation, so pinning
            // the `&mut` reborrow is sound.
            let fut = unsafe { Pin::new_unchecked(&mut *fut) };
            let poll = match std::panic::catch_unwind(AssertUnwindSafe(|| {
                let waker = crate::waker::lend(waker);
                let mut context = Context::from_waker(&waker);
                fut.poll(&mut context)
            })) {
                Ok(poll) => poll,
                Err(payload) => Poll::Ready(Err(poll_panic_exception(&*payload))),
            };
            match poll {
                Poll::Ready(Ok(value)) => {
                    // SAFETY: `ret` points to storage suitable for a `T` (Complete arm).
                    unsafe { std::ptr::write(ret.cast::<T>(), value) };
                    FuturePollStatus::COMPLETE
                }
                // SAFETY: `ret` has room for a `kj::Exception*`, as for a `T`.
                Poll::Ready(Err(error)) => unsafe { write_error(ret, error) },
                Poll::Pending => FuturePollStatus::PENDING,
            }
        }

        /// # Safety
        ///
        /// C++ `RustFuture` vtable protocol (future.h): `fut` must be the `fut` field of a
        /// live `RustFuture<T>` created by [`future`], and must not be used again afterwards
        /// (drop-exactly-once, enforced by `Impl`'s move semantics on the C++ side).
        ///
        /// Unwind safety: a panic in the future's destructor has no error channel (this is
        /// called from C++ destructors/cancellation paths), so it is logged rather than
        /// unwound into C++ ([`drop_catching_panic`]).
        pub(crate) unsafe extern "C" fn drop_in_place(fut: *mut c_void) {
            // SAFETY: per this fn's `# Safety` contract, `fut` is the `fut` field of a live
            // `RustFuture<T>` not used again, so we may read the pointer, reclaim the box, and pin.
            let fut = unsafe { *(fut.cast::<FuturePtr<T>>()) };
            // SAFETY: `fut` was produced by `Box::into_raw` in [`future`]; reclaim ownership once.
            let fut = unsafe { Box::from_raw(fut) };
            // SAFETY: the box is never moved out of its allocation, so pinning it is sound.
            let fut = unsafe { Pin::new_unchecked(fut) };
            drop_catching_panic(fut);
        }
    }

    impl<T: Unpin> RustInfallibleFuture<'_, T> {
        /// # Safety
        ///
        /// Same contract as [`RustFuture::poll`], with `fut` created by [`infallible_future`].
        /// Although the future itself cannot return an error, a panic escaping its `poll` is
        /// still converted into an errored completion (`FuturePollStatus::ERROR` writing a
        /// `kj::Exception*`): the C++ `FuturePoller<T>` handles the Error arm identically for
        /// infallible futures, and `kj::Promise<T>` can always carry an exception.
        pub(crate) unsafe extern "C" fn poll(
            fut: *mut c_void,
            waker: &PollWaker,
            ret: *mut c_void,
        ) -> FuturePollStatus {
            // SAFETY: per this fn's `# Safety` contract, `fut` is the `fut` field of a live
            // `RustInfallibleFuture<T>`, i.e. a valid `*mut InfallibleFuturePtr<T>` to read+pin.
            let fut = unsafe { *(fut.cast::<InfallibleFuturePtr<T>>()) };
            // SAFETY: the boxed future is never moved out of its allocation, so pinning is sound.
            let fut = unsafe { Pin::new_unchecked(&mut *fut) };
            match std::panic::catch_unwind(AssertUnwindSafe(|| {
                let waker = crate::waker::lend(waker);
                let mut context = Context::from_waker(&waker);
                fut.poll(&mut context)
            })) {
                Ok(Poll::Ready(value)) => {
                    // SAFETY: `ret` points to storage suitable for a `T` (Complete arm).
                    unsafe { std::ptr::write(ret.cast::<T>(), value) };
                    FuturePollStatus::COMPLETE
                }
                Ok(Poll::Pending) => FuturePollStatus::PENDING,
                // SAFETY: `ret` has room for a `kj::Exception*`, as for a `T`.
                Err(payload) => unsafe { write_error(ret, poll_panic_exception(&*payload)) },
            }
        }

        /// # Safety
        ///
        /// Same contract as [`RustFuture::drop_in_place`], with `fut` created by
        /// [`infallible_future`]. A panic in the destructor is logged (see there).
        pub(crate) unsafe extern "C" fn drop_in_place(fut: *mut c_void) {
            // SAFETY: per this fn's `# Safety` contract, `fut` is the `fut` field of a live
            // `RustInfallibleFuture<T>` not used again; read the pointer, reclaim the box, and pin.
            let fut = unsafe { *(fut.cast::<InfallibleFuturePtr<T>>()) };
            // SAFETY: `fut` came from `Box::into_raw` in [`infallible_future`]; reclaim once.
            let fut = unsafe { Box::from_raw(fut) };
            // SAFETY: the box is never moved out of its allocation, so pinning it is sound.
            let fut = unsafe { Pin::new_unchecked(fut) };
            drop_catching_panic(fut);
        }
    }

    /// Writes `error` to `ret` for `FuturePoller` in future.h, which takes it over.
    ///
    /// # Safety
    /// `ret` must be the output of a poll callback, which has room for a `kj::Exception*`.
    unsafe fn write_error(ret: *mut c_void, error: cxx::KjException) -> FuturePollStatus {
        // Safety: forwarded from the caller's contract.
        unsafe { std::ptr::write(ret.cast::<*mut c_void>(), error.into_raw().as_ptr().cast()) };
        FuturePollStatus::ERROR
    }

    /// The exception that fails the promise of a future whose `poll()` panicked with `payload`.
    ///
    /// A `CanceledException` payload fails it too, as a `FAILED` exception, since a promise
    /// rejects only with a `kj::Exception`.
    #[cold]
    fn poll_panic_exception(payload: &(dyn std::any::Any + Send)) -> cxx::KjException {
        cxx::KjError::new(
            cxx::KjExceptionType::Failed,
            cxx::private::panic_description("Rust future", payload),
        )
        .into()
    }

    /// Drops `fut`, logging a panic in its destructor rather than unwinding into C++, which drops
    /// the future when it cancels its promise.
    fn drop_catching_panic<F: ?Sized>(fut: Pin<Box<F>>) {
        if let Err(payload) = std::panic::catch_unwind(AssertUnwindSafe(|| drop(fut))) {
            log_drop_panic(&*payload);
        }
    }

    #[cold]
    fn log_drop_panic(payload: &(dyn std::any::Any + Send)) {
        let description = cxx::private::panic_description("Rust future destructor", payload);
        // Safety: the pointer and length describe `description`, which outlives the call.
        unsafe { log_error(description.as_ptr(), description.len()) };
    }

    unsafe extern "C" {
        /// `KJ_LOG(ERROR, ...)` with the UTF-8 message of `len` bytes at `ptr`.
        #[link_name = "cxxbridge$kjrs$future$logError"]
        fn log_error(ptr: *const u8, len: usize);
    }

    #[must_use]
    pub fn future<'a, T: Unpin>(
        fut: Pin<Box<dyn Future<Output = Result<T, cxx::KjException>> + 'a>>,
    ) -> RustFuture<'a, T> {
        // SAFETY: the box is immediately re-boxed via `Box::into_raw` and only ever reconstituted
        // (and re-pinned) in `drop_in_place`, so the pinned future is never moved.
        let fut = Box::into_raw(unsafe { Pin::into_inner_unchecked(fut) });
        let poll = RustFuture::<T>::poll;
        let drop = RustFuture::<T>::drop_in_place;
        RustFuture { fut, poll, drop }
    }

    #[must_use]
    pub fn infallible_future<'a, T: Unpin>(
        fut: Pin<Box<dyn Future<Output = T> + 'a>>,
    ) -> RustInfallibleFuture<'a, T> {
        // SAFETY: the box is immediately re-boxed via `Box::into_raw` and only ever reconstituted
        // (and re-pinned) in `drop_in_place`, so the pinned future is never moved.
        let fut = Box::into_raw(unsafe { Pin::into_inner_unchecked(fut) });
        let poll = RustInfallibleFuture::<T>::poll;
        let drop = RustInfallibleFuture::<T>::drop_in_place;
        RustInfallibleFuture { fut, poll, drop }
    }
}

// A future that converts error into `cxx::KjException`
struct MapErr<F> {
    fut: F,
    file: &'static str,
    line: u32,
}

impl<F, T, E> Future for MapErr<F>
where
    F: Future<Output = Result<T, E>>,
    E: IntoKjException,
{
    type Output = Result<T, cxx::KjException>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let file = self.file;
        let line = self.line;

        // Safety: self is pinned, so fut is pinned.
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
        let inner: Pin<&mut F> = unsafe {
            let this = self.get_unchecked_mut();
            Pin::new_unchecked(&mut this.fut)
        };
        match inner.poll(cx) {
            Poll::Pending => Poll::Pending,
            Poll::Ready(Ok(value)) => Poll::Ready(Ok(value)),
            Poll::Ready(Err(e)) => Poll::Ready(Err(::cxx::IntoKjException::into_kj_exception(
                e, file, line,
            ))),
        }
    }
}

/// Convert a `Future` using any `IntoKjException` error into `cxx::KjException` one.
pub fn map_err<F, T, E>(
    fut: F,
    file: &'static str,
    line: u32,
) -> impl Future<Output = Result<T, cxx::KjException>>
where
    F: Future<Output = Result<T, E>>,
    E: IntoKjException,
{
    MapErr { fut, file, line }
}
