//! Loop-runtime precondition for creating tokio resources.
//!
//! Every tokio resource kj-rs-io creates (sockets, listeners, `AsyncFd`s, signal streams, the
//! blocking DNS task) registers with the runtime that is *current* on the creating thread. On a
//! thread owning a `kj_rs_tokio::TokioEventPort` that is normally the port's runtime: the thread
//! stays inside its context for the port's whole life (kj-rs-tokio's `EnteredRuntime`), so no
//! per-call `Handle::enter()` is needed, and resources are then driven while the KJ loop parks in
//! the port's `wait()`/`poll()`.
//!
//! [`require_loop_runtime`] is checked once, at resource creation (never per read or write:
//! polling an existing resource needs no runtime context, tokio's own I/O types do the same). It
//! checks two things and turns either failure into a `kj::Exception` with a useful message
//! instead of the panic or silent misregistration tokio would produce:
//!
//! 1. this thread owns a `TokioEventPort` (there is a loop runtime at all), and
//! 2. the runtime tokio would register a new resource with -- the *currently entered* one -- is
//!    that port's runtime, by identity (`Handle::id`). Code on the loop thread that has entered
//!    an auxiliary runtime's context (`Handle::enter()`) and creates a kj-rs-io resource inside
//!    that scope would otherwise register it with a driver the KJ loop never turns.
//!
//! What it still cannot check: a native tokio socket handed to another thread (`ServeIo::Tcp`)
//! stays registered with the loop that created it -- see `serve.rs`.

use crate::error::KjIoError;
use crate::error::Result;

/// Errors (`kj::Exception`-convertible) unless this thread owns a `TokioEventPort` whose
/// runtime is the one currently entered.
pub fn require_loop_runtime() -> Result<()> {
    let Some(port_runtime) = kj_rs_tokio::current_handle() else {
        return Err(KjIoError::other(
            "kj_rs_io",
            "no kj-rs-tokio runtime on this thread; kj-rs-io requires a TokioEventPort \
             (see kj_rs_io::setupTokioAsyncIo())",
        ));
    };
    match tokio::runtime::Handle::try_current() {
        Ok(current) if current.id() == port_runtime.id() => Ok(()),
        Ok(_) => Err(KjIoError::other(
            "kj_rs_io",
            "a different tokio runtime is entered on this thread; kj-rs-io resources must be \
             created inside the TokioEventPort's runtime context, or the KJ loop cannot drive them",
        )),
        Err(_) => Err(KjIoError::other(
            "kj_rs_io",
            "the TokioEventPort's runtime context is not entered on this thread",
        )),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn require_loop_runtime_errors_without_a_port() {
        // A thread with no TokioEventPort has no loop runtime; the precondition must be a
        // kj::Exception-convertible error naming it, not a tokio panic.
        let err = cxx::KjError::from(require_loop_runtime().unwrap_err());
        assert!(
            err.description()
                .contains("no kj-rs-tokio runtime on this thread"),
            "{}",
            err.description()
        );
    }

    #[test]
    fn require_loop_runtime_passes_with_a_port() {
        let _port = kj_rs_tokio::TokioPort::new();
        assert!(require_loop_runtime().is_ok());
    }

    /// Entering an auxiliary runtime on the loop thread must be rejected: a resource created
    /// there would register with that runtime's driver, which the KJ loop never turns.
    #[test]
    fn require_loop_runtime_rejects_a_foreign_entered_runtime() {
        let _port = kj_rs_tokio::TokioPort::new();
        let auxiliary = tokio::runtime::Builder::new_current_thread()
            .build()
            .unwrap();
        let guard = auxiliary.enter();
        let err = cxx::KjError::from(require_loop_runtime().unwrap_err());
        assert!(
            err.description().contains("a different tokio runtime"),
            "{}",
            err.description()
        );
        drop(guard);
        assert!(require_loop_runtime().is_ok(), "back in the port's context");
    }
}
