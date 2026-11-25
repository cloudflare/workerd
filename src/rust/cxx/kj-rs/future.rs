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
    use std::task::Waker;

    use static_assertions::assert_eq_size;

    use super::FuturePollStatus;
    use crate::KjWaker;

    type PollCallback = unsafe extern "C" fn(
        fut: *mut c_void,
        waker: *const c_void,
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
        pub(crate) unsafe extern "C" fn poll(
            fut: *mut c_void,
            waker: *const c_void,
            ret: *mut c_void,
        ) -> FuturePollStatus {
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { *(fut.cast::<FuturePtr<T>>()) };
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { Pin::new_unchecked(&mut *fut) };
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let waker = unsafe { &*waker.cast::<KjWaker>() };
            let waker = Waker::from(waker);
            let mut context = Context::from_waker(&waker);
            let poll = match std::panic::catch_unwind(AssertUnwindSafe(|| fut.poll(&mut context))) {
                Ok(poll) => poll,
                Err(payload) => Poll::Ready(Err(poll_panic_exception(&*payload))),
            };
            match poll {
                Poll::Ready(Ok(value)) => {
                    // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
                    unsafe { std::ptr::write(ret.cast::<T>(), value) };
                    FuturePollStatus::COMPLETE
                }
                // Safety: `ret` has room for a `kj::Exception*`, as for a `T`.
                Poll::Ready(Err(error)) => unsafe { write_error(ret, error) },
                Poll::Pending => FuturePollStatus::PENDING,
            }
        }

        pub(crate) unsafe extern "C" fn drop_in_place(fut: *mut c_void) {
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { *(fut.cast::<FuturePtr<T>>()) };
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { Box::from_raw(fut) };
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { Pin::new_unchecked(fut) };
            drop_catching_panic(fut);
        }
    }

    impl<T: Unpin> RustInfallibleFuture<'_, T> {
        pub(crate) unsafe extern "C" fn poll(
            fut: *mut c_void,
            waker: *const c_void,
            ret: *mut c_void,
        ) -> FuturePollStatus {
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { *(fut.cast::<InfallibleFuturePtr<T>>()) };
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { Pin::new_unchecked(&mut *fut) };
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let waker = unsafe { &*waker.cast::<KjWaker>() };
            let waker = Waker::from(waker);
            let mut context = Context::from_waker(&waker);
            match std::panic::catch_unwind(AssertUnwindSafe(|| fut.poll(&mut context))) {
                Ok(Poll::Ready(value)) => {
                    // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
                    unsafe { std::ptr::write(ret.cast::<T>(), value) };
                    FuturePollStatus::COMPLETE
                }
                Ok(Poll::Pending) => FuturePollStatus::PENDING,
                // Safety: `ret` has room for a `kj::Exception*`, as for a `T`.
                Err(payload) => unsafe { write_error(ret, poll_panic_exception(&*payload)) },
            }
        }

        pub(crate) unsafe extern "C" fn drop_in_place(fut: *mut c_void) {
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { *(fut.cast::<InfallibleFuturePtr<T>>()) };
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { Box::from_raw(fut) };
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
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
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
        let fut = Box::into_raw(unsafe { Pin::into_inner_unchecked(fut) });
        let poll = RustFuture::<T>::poll;
        let drop = RustFuture::<T>::drop_in_place;
        RustFuture { fut, poll, drop }
    }

    #[must_use]
    pub fn infallible_future<'a, T: Unpin>(
        fut: Pin<Box<dyn Future<Output = T> + 'a>>,
    ) -> RustInfallibleFuture<'a, T> {
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
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
