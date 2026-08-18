//! FFI island (see crate-root `#![deny(unsafe_code)]`): the `RustFuture` C-ABI vtable that drives
//! all bridged async — `unsafe extern "C"` poll/drop callbacks, raw-pointer result writes, and
//! `Pin`/`Box` raw conversions. A genuine unsafe seam.
#![allow(unsafe_code)]

// This file contains boilerplate which must occur once per crate, rather than once per type.

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
    use std::pin::Pin;
    use std::task::Context;
    use std::task::Poll;
    use std::task::Waker;

    use static_assertions::assert_eq_size;

    use super::FuturePollStatus;
    use crate::ffi::PollWaker;

    /// Converts a panic payload (from `std::panic::catch_unwind`) escaping a bridged future
    /// into a heap-allocated `kj::Exception` written to the poll callback's output parameter,
    /// so C++ observes a rejected promise instead of a process abort.
    ///
    /// This mirrors the sync bridge path (`cxx::private::try_unwind`/`catch_unwind` in
    /// src/unwind.rs), which converts panics in `extern "Rust"` functions into
    /// `kj::Exception`s. One divergence: a `cxx::CanceledException` payload (produced when an
    /// infallible `extern "C++"` call throws `kj::CanceledException`) cannot be propagated as
    /// a distinct "canceled" state here, because `FuturePollStatus` has no Canceled arm; it is
    /// reported as a regular `kj::Exception` describing the cancellation instead.
    ///
    /// # Safety
    ///
    /// `ret` must point to storage valid for holding a `kj::Exception*` (the C++
    /// `FuturePoller<T>` union guarantees this for the Error arm).
    #[expect(
        clippy::needless_pass_by_value,
        reason = "takes ownership of the panic payload, mirroring std::panic::catch_unwind's Err arm"
    )]
    unsafe fn write_panic_as_exception(
        ret: *mut c_void,
        err: Box<dyn std::any::Any + Send>,
    ) -> FuturePollStatus {
        let msg = if let Some(s) = err.downcast_ref::<&'static str>() {
            format!("panic in bridged future poll: {s}")
        } else if let Some(s) = err.downcast_ref::<String>() {
            format!("panic in bridged future poll: {s}")
        } else if err.downcast_ref::<cxx::CanceledException>().is_some() {
            "panic in bridged future poll: kj::CanceledException".to_owned()
        } else {
            "panic in bridged future poll".to_owned()
        };
        let exception = cxx::IntoKjException::into_kj_exception(
            cxx::KjError::new(cxx::KjExceptionType::Failed, msg),
            file!(),
            line!(),
        );
        // SAFETY: `ret` points to storage valid for a `kj::Exception*` per this fn's
        // `# Safety` contract (the C++ `FuturePoller<T>` Error arm).
        unsafe {
            std::ptr::write(
                ret.cast::<*mut c_void>(),
                exception.into_raw().as_ptr().cast(),
            );
        }
        FuturePollStatus::ERROR
    }

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
        pub(crate) unsafe extern "C" fn poll(
            fut: *mut c_void,
            waker: &PollWaker,
            ret: *mut c_void,
        ) -> FuturePollStatus {
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { *(fut.cast::<FuturePtr<T>>()) };
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { Pin::new_unchecked(&mut *fut) };
            let waker = Waker::from(waker);
            let mut context = Context::from_waker(&waker);
            match fut.poll(&mut context) {
                Poll::Ready(Ok(value)) => {
                    // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
                    unsafe { std::ptr::write(ret.cast::<T>(), value) };
                    FuturePollStatus::COMPLETE
                }
                Poll::Ready(Err(error)) => {
                    // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
                    unsafe {
                        std::ptr::write(
                            ret.cast::<*mut c_void>(),
                            error.into_raw().as_ptr().cast(),
                        );
                    };
                    FuturePollStatus::ERROR
                }
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
            drop(fut);
        }
    }

    impl<T: Unpin> RustInfallibleFuture<'_, T> {
        pub(crate) unsafe extern "C" fn poll(
            fut: *mut c_void,
            waker: &PollWaker,
            ret: *mut c_void,
        ) -> FuturePollStatus {
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { *(fut.cast::<InfallibleFuturePtr<T>>()) };
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { Pin::new_unchecked(&mut *fut) };
            let waker = Waker::from(waker);
            let mut context = Context::from_waker(&waker);
            match fut.poll(&mut context) {
                Poll::Ready(value) => {
                    // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
                    unsafe { std::ptr::write(ret.cast::<T>(), value) };
                    FuturePollStatus::COMPLETE
                }
                Poll::Pending => FuturePollStatus::PENDING,
            }
        }

        pub(crate) unsafe extern "C" fn drop_in_place(fut: *mut c_void) {
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { *(fut.cast::<InfallibleFuturePtr<T>>()) };
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { Box::from_raw(fut) };
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            let fut = unsafe { Pin::new_unchecked(fut) };
            drop(fut);
        }
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
