#![allow(missing_docs)]
#![allow(dead_code)]

use alloc::string::String;
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
        Err(err) => panic_result(label, &*err),
    }
}

/// Run the error-returning `foreign_call`, intercepting panics and converting them to errors.
pub fn try_unwind<F>(label: &'static str, foreign_call: F) -> Result
where
    F: FnOnce() -> Result,
{
    match panic::catch_unwind(panic::AssertUnwindSafe(foreign_call)) {
        Ok(r) => r,
        Err(err) => panic_result(label, &*err),
    }
}

fn panic_result(label: &'static str, err: &(dyn core::any::Any + Send)) -> Result {
    if err.is::<CanceledException>() {
        return Result::canceled();
    }

    Result::error(
        KjError::new(KjExceptionType::Failed, panic_description(label, err)),
        file!(),
        line!(),
    )
}

/// Describes a panic with `payload` that happened in `label`, as `panic in {label}`, followed by
/// the panic's message when it has one.
#[cold]
pub fn panic_description(label: &str, payload: &(dyn core::any::Any + Send)) -> String {
    match panic_message(payload) {
        Some(message) => std::format!("panic in {label}: {message}"),
        None => std::format!("panic in {label}"),
    }
}

/// The message of a panic with `payload`: the string that `panic!()` and `panic_any()` take, or
/// the description of a `CanceledException`.
pub fn panic_message(payload: &(dyn core::any::Any + Send)) -> Option<&str> {
    if let Some(message) = payload.downcast_ref::<String>() {
        Some(message)
    } else if let Some(message) = payload.downcast_ref::<&'static str>() {
        Some(message)
    } else if payload.is::<CanceledException>() {
        Some("kj::CanceledException")
    } else {
        None
    }
}
