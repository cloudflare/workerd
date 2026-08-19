//! KJ ffi crate.
//!
//! This crate provides bindings to common KJ classes and functions that do not require special
//! C++ bridge support.

pub mod http;
pub mod io;
pub mod macros;

mod own;

// Re-exported so that `$crate::KjError` / `$crate::KjExceptionType` resolve inside the
// macros in `macros.rs` regardless of whether the invoking crate itself depends on `cxx`.
pub use cxx::KjError;
pub use cxx::KjExceptionType;
pub use own::*;

pub type Result<T> = std::result::Result<T, cxx::KjError>;
