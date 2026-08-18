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
