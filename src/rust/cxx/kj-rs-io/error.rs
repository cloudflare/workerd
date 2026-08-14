//! Error mapping: `std::io::Error` -> `kj::Exception`, preserving KJ's exception-type taxonomy.
//!
//! kj-http and capnp RPC change behavior based on `kj::Exception::Type` (e.g. `DISCONNECTED`
//! failures are treated as clean peer hangups rather than bugs), so the mapping of connection
//! errors matters for behavioral parity with `kj::setupAsyncIo()`.

use cxx::IntoKjException;
use cxx::KjError;
use cxx::KjException;
use cxx::KjExceptionType;

pub type Result<T> = std::result::Result<T, KjIoError>;

/// An `std::io::Error` (plus operation context) that converts into a `kj::Exception` with an
/// appropriate exception type.
#[derive(Debug)]
pub struct KjIoError {
    /// Name of the failing operation, included in the exception description the way KJ's
    /// `KJ_SYSCALL` includes the syscall name (e.g. "`connect()`: Connection refused ...").
    op: &'static str,
    inner: std::io::Error,
}

impl KjIoError {
    pub(crate) fn other(op: &'static str, message: impl std::fmt::Display) -> Self {
        Self {
            op,
            inner: std::io::Error::other(message.to_string()),
        }
    }
}

/// Attaches an operation name to `io::Error`s, for use with `Result::map_err`.
pub fn op(name: &'static str) -> impl Fn(std::io::Error) -> KjIoError {
    move |inner| KjIoError { op: name, inner }
}

fn exception_type(error: &std::io::Error) -> KjExceptionType {
    use std::io::ErrorKind;
    // Primary classification: by raw errno, mirroring KJ's own table (`typeOfErrno()` in
    // kj/debug.c++) errno-for-errno. Consumers (kj-http, capnp-rpc) change behavior on the
    // exception type, so the classes must match `kj::setupAsyncIo()` exactly — e.g. ETIMEDOUT
    // is OVERLOADED in KJ (retry-later), NOT DISCONNECTED (clean peer hangup), and std's
    // `ErrorKind` buckets have no stable kinds at all for KJ's fd/memory-exhaustion OVERLOADED
    // set (EMFILE/ENFILE/ENOBUFS/...), hence the raw match.
    #[cfg(unix)]
    if let Some(errno) = error.raw_os_error() {
        return errno_exception_type(errno);
    }
    // Fallback for synthetic (non-OS) errors — and all errors on Windows, where
    // `raw_os_error()` is a Win32/WSA code, not an errno (KJ classifies those in
    // `typeOfWin32Error()`; the buckets below agree with it for the kinds tokio surfaces).
    match error.kind() {
        // KJ's DISCONNECTED class: connection teardown, treated as a clean peer hangup.
        ErrorKind::ConnectionRefused
        | ErrorKind::ConnectionReset
        | ErrorKind::ConnectionAborted
        | ErrorKind::BrokenPipe
        | ErrorKind::NotConnected
        | ErrorKind::UnexpectedEof
        | ErrorKind::HostUnreachable
        | ErrorKind::NetworkUnreachable
        | ErrorKind::NetworkDown => KjExceptionType::Disconnected,
        // KJ's OVERLOADED class: temporary lack of resources (ETIMEDOUT/WSAETIMEDOUT and
        // ENOMEM land here in KJ's tables).
        ErrorKind::TimedOut | ErrorKind::OutOfMemory => KjExceptionType::Overloaded,
        ErrorKind::Unsupported => KjExceptionType::Unimplemented,
        _ => KjExceptionType::Failed,
    }
}

/// Exact mirror of KJ's `typeOfErrno()` (kj/debug.c++), so `kj::Exception::Type` matches the
/// native `kj::setupAsyncIo()` backend errno-for-errno.
#[cfg(unix)]
fn errno_exception_type(errno: i32) -> KjExceptionType {
    // Errnos that are `#ifdef`-conditional in KJ's table for platform reasons, mirrored here
    // with `cfg`: ENONET exists only on Linux; EOPNOTSUPP aliases ENOTSUP on Linux (KJ compiles
    // its case only `#if EOPNOTSUPP != ENOTSUP` — an or-pattern with both would be an
    // unreachable pattern there).
    #[cfg(any(target_os = "linux", target_os = "android"))]
    if errno == libc::ENONET {
        return KjExceptionType::Disconnected;
    }
    #[cfg(not(any(target_os = "linux", target_os = "android")))]
    if errno == libc::EOPNOTSUPP {
        return KjExceptionType::Unimplemented;
    }
    match errno {
        // OVERLOADED: the call failed because of a temporary lack of resources.
        libc::EDQUOT
        | libc::EMFILE
        | libc::ENFILE
        | libc::ENOBUFS
        | libc::ENOLCK
        | libc::ENOMEM
        | libc::ENOSPC
        | libc::ETIMEDOUT
        | libc::EUSERS => KjExceptionType::Overloaded,
        // DISCONNECTED: communication over a connection that has been lost.
        libc::ENOTCONN
        | libc::ECONNABORTED
        | libc::ECONNREFUSED
        | libc::ECONNRESET
        | libc::EHOSTDOWN
        | libc::EHOSTUNREACH
        | libc::ENETDOWN
        | libc::ENETRESET
        | libc::ENETUNREACH
        | libc::EPIPE => KjExceptionType::Disconnected,
        // UNIMPLEMENTED: the "not supported" family (ENOTSOCK is really "syscall not
        // implemented for non-sockets", per KJ's own comment).
        libc::ENOSYS | libc::ENOTSUP | libc::ENOPROTOOPT | libc::ENOTSOCK => {
            KjExceptionType::Unimplemented
        }
        _ => KjExceptionType::Failed,
    }
}

impl From<KjIoError> for KjError {
    fn from(error: KjIoError) -> Self {
        let description = format!("{}: {}", error.op, error.inner);
        Self::new(exception_type(&error.inner), description)
    }
}

impl IntoKjException for KjIoError {
    fn into_kj_exception(self, file: &str, line: u32) -> KjException {
        KjError::from(self).into_kj_exception(file, line)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn kind_error(kind: std::io::ErrorKind) -> std::io::Error {
        std::io::Error::new(kind, "synthetic")
    }

    #[test]
    fn errorkind_fallback_matches_kj_classes() {
        use std::io::ErrorKind;
        for kind in [
            ErrorKind::ConnectionRefused,
            ErrorKind::ConnectionReset,
            ErrorKind::ConnectionAborted,
            ErrorKind::BrokenPipe,
            ErrorKind::NotConnected,
            ErrorKind::UnexpectedEof,
        ] {
            assert_eq!(
                exception_type(&kind_error(kind)),
                KjExceptionType::Disconnected,
                "{kind:?}"
            );
        }
        assert_eq!(
            exception_type(&kind_error(ErrorKind::TimedOut)),
            KjExceptionType::Overloaded
        );
        assert_eq!(
            exception_type(&kind_error(ErrorKind::OutOfMemory)),
            KjExceptionType::Overloaded
        );
        assert_eq!(
            exception_type(&kind_error(ErrorKind::Unsupported)),
            KjExceptionType::Unimplemented
        );
        assert_eq!(
            exception_type(&kind_error(ErrorKind::Other)),
            KjExceptionType::Failed
        );
        assert_eq!(
            exception_type(&kind_error(ErrorKind::InvalidInput)),
            KjExceptionType::Failed
        );
    }

    /// The raw-errno table must mirror KJ's `typeOfErrno()` errno-for-errno, since consumers
    /// (kj-http, capnp-rpc) change behavior on the class. Spot-checks one member of every
    /// class plus the classic confusables (ETIMEDOUT is OVERLOADED, not DISCONNECTED).
    #[cfg(unix)]
    #[test]
    fn errno_table_matches_kj() {
        let of = |errno: i32| exception_type(&std::io::Error::from_raw_os_error(errno));
        // OVERLOADED
        for errno in [
            libc::EMFILE,
            libc::ENFILE,
            libc::ENOBUFS,
            libc::ENOMEM,
            libc::ETIMEDOUT,
        ] {
            assert_eq!(of(errno), KjExceptionType::Overloaded, "errno {errno}");
        }
        // DISCONNECTED
        for errno in [
            libc::ECONNREFUSED,
            libc::ECONNRESET,
            libc::EPIPE,
            libc::ENOTCONN,
            libc::EHOSTUNREACH,
            libc::ENETDOWN,
        ] {
            assert_eq!(of(errno), KjExceptionType::Disconnected, "errno {errno}");
        }
        // UNIMPLEMENTED
        for errno in [
            libc::ENOSYS,
            libc::ENOTSUP,
            libc::ENOPROTOOPT,
            libc::ENOTSOCK,
        ] {
            assert_eq!(of(errno), KjExceptionType::Unimplemented, "errno {errno}");
        }
        // FAILED (everything else)
        for errno in [libc::EINVAL, libc::EACCES, libc::EBADF, libc::EEXIST] {
            assert_eq!(of(errno), KjExceptionType::Failed, "errno {errno}");
        }
        // Platform-conditional entries mirror KJ's #ifdefs.
        #[cfg(any(target_os = "linux", target_os = "android"))]
        assert_eq!(of(libc::ENONET), KjExceptionType::Disconnected);
        #[cfg(not(any(target_os = "linux", target_os = "android")))]
        assert_eq!(of(libc::EOPNOTSUPP), KjExceptionType::Unimplemented);
    }

    #[test]
    fn description_is_op_colon_message() {
        let err = KjIoError::other("connect()", "boom");
        let kj = KjError::from(err);
        assert_eq!(kj.description(), "connect(): boom");
        assert_eq!(kj.exception_type(), KjExceptionType::Failed);

        let err = op("read()")(kind_error(std::io::ErrorKind::ConnectionReset));
        let kj = KjError::from(err);
        assert!(
            kj.description().starts_with("read(): "),
            "{}",
            kj.description()
        );
        assert_eq!(kj.exception_type(), KjExceptionType::Disconnected);
    }
}
