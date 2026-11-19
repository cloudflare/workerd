#![allow(missing_docs)]
#![allow(dead_code)]

use core::ffi::c_void;
use core::mem;
use std::panic;

use crate::CanceledException;
use crate::KjError;
use crate::KjExceptionType;
use crate::result::Result;

pub fn prevent_unwind<F, R>(label: &'static str, foreign_call: F) -> R
where
    F: FnOnce() -> R,
{
    // Goal is to make it impossible to propagate a panic across the C interface
    // of an extern "Rust" function, which would be Undefined Behavior. We
    // transform such panicks into a deterministic abort instead. When cxx is
    // built in an application using panic=abort, this guard object is compiled
    // out because its destructor is statically unreachable. When built with
    // panic=unwind, an unwind from the foreign call will attempt to drop the
    // guard object leading to a double panic, which is defined by Rust to
    // abort. In no_std programs, on most platforms the current mechanism for
    // this is for core::intrinsics::abort to invoke an invalid instruction. On
    // Unix, the process will probably terminate with a signal like SIGABRT,
    // SIGILL, SIGTRAP, SIGSEGV or SIGBUS. The precise behaviour is not
    // guaranteed and not stable, but is safe.
    let guard = Guard { label };

    let ret = foreign_call();

    // If we made it here, no uncaught panic occurred during the foreign call.
    mem::forget(guard);
    ret
}

struct Guard {
    label: &'static str,
}

impl Drop for Guard {
    #[cold]
    fn drop(&mut self) {
        panic!("panic in ffi function {}, aborting.", self.label);
    }
}

/// Run the void `foreign_call`, intercepting panics and converting them to errors.
pub fn catch_unwind<F>(label: &'static str, foreign_call: F) -> Result
where
    F: FnOnce(),
{
    match panic::catch_unwind(panic::AssertUnwindSafe(foreign_call)) {
        Ok(()) => Result::ok(),
        Err(err) => panic_result(label, err),
    }
}

/// Run the error-returning `foreign_call`, intercepting panics and converting them to errors.
pub fn try_unwind<F>(label: &'static str, foreign_call: F) -> Result
where
    F: FnOnce() -> Result,
{
    match panic::catch_unwind(panic::AssertUnwindSafe(foreign_call)) {
        Ok(r) => r,
        Err(err) => panic_result(label, err),
    }
}

fn panic_result(label: &'static str, err: alloc::boxed::Box<dyn core::any::Any + Send>) -> Result {
    if let Some(err) = err.downcast_ref::<alloc::string::String>() {
        return Result::error(
            KjError::new(
                KjExceptionType::Failed,
                std::format!("panic in {label}: {err}"),
            ),
            file!(),
            line!(),
        );
    } else if err.downcast::<CanceledException>().is_ok() {
        return Result::canceled();
    }

    Result::error(
        KjError::new(KjExceptionType::Failed, std::format!("panic in {label}")),
        file!(),
        line!(),
    )
}

/// Runs `f`, and panics if a C++ exception unwinds out of it, with the panic that an infallible
/// `extern "C++"` function raises when it throws.
///
/// Such an exception comes from a `#[nothrow]` function, whose bridge function does not catch.
/// Rust cannot catch it either: at `catch_unwind`, or at a function that cannot unwind, it
/// aborts the process. Run the code that calls `#[nothrow]` functions through this, inside the
/// `catch_unwind` that handles its panics, and such an exception is handled as a panic instead.
///
/// # Panics
///
/// Panics if a C++ exception unwinds out of `f`, with a `CanceledException` payload if it is a
/// `kj::CanceledException`. A panic in `f` propagates unchanged.
#[track_caller]
pub fn panic_on_exception<F, R>(f: F) -> R
where
    F: FnOnce() -> R,
{
    unsafe extern "C-unwind" {
        #[link_name = "cxxbridge1$catch_exception"]
        fn catch_exception(
            call: unsafe extern "C-unwind" fn(*mut c_void),
            data: *mut c_void,
        ) -> Result;
    }

    unsafe extern "C-unwind" fn call<F, R>(data: *mut c_void)
    where
        F: FnOnce() -> R,
    {
        // Safety: `data` is the `(f, result)` of the `panic_on_exception` that passed it.
        let (f, result) = unsafe { &mut *data.cast::<(Option<F>, Option<R>)>() };
        if let Some(f) = f.take() {
            *result = Some(f());
        }
    }

    let mut data = (Some(f), None);
    // Safety: `call::<F, R>` reads `data`, which outlives the call, as the type it has.
    unsafe { catch_exception(call::<F, R>, (&raw mut data).cast()) }.panic_on_exception();
    match data.1 {
        Some(result) => result,
        None => unreachable!(),
    }
}
