//! KJ ffi crate.
//!
//! This crate provides bindings to common KJ classes and functions that do not require special
//! C++ bridge support.

pub mod http;
pub mod io;
pub mod random;

mod own;
pub use own::*;

pub type Result<T> = std::result::Result<T, cxx::KjError>;
