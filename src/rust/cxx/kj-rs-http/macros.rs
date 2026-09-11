// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Macros for building [`cxx::KjError`] values, mirroring the C++ `KJ_EXCEPTION`/`KJ_FAIL_REQUIRE`/
//! `KJ_UNIMPLEMENTED` macros defined in `kj/debug.h`.
//!
//! Each of KJ's four [`cxx::KjExceptionType`] variants (`Failed`, `Overloaded`, `Disconnected`,
//! `Unimplemented`) gets two macros:
//!
//! - A construct-only macro (`failed!`, `overloaded!`, `disconnected!`, `not_implemented!`) that
//!   evaluates to a [`cxx::KjError`] value, mirroring C++ `KJ_EXCEPTION(TYPE, ...)`. The caller
//!   decides what to do with it (`Err(...)`, `.into()`, attach `.with_location(...)`, etc.).
//! - An early-return macro (`fail_require!`, `overloaded_require!`, `disconnected_require!`,
//!   `not_implemented_require!`) that unconditionally returns `Err(...)` from the enclosing
//!   function, mirroring C++ `KJ_FAIL_REQUIRE(...)` / `KJ_UNIMPLEMENTED(...)`. (`unimplemented!`
//!   is avoided as a macro name here since it would shadow `std::unimplemented!` wherever
//!   imported.)

/// Builds a [`cxx::KjError`] with [`cxx::KjExceptionType::Failed`] and a `format!`-style message.
///
/// This is the Rust equivalent of the C++ `KJ_EXCEPTION(FAILED, ...)` macro defined in
/// `kj/debug.h`. Unlike `KJ_FAIL_REQUIRE`, this only *constructs* the error; it does not return
/// or throw. Wrap the result in `Err(...)` (or use [`fail_require!`] for the early-return form).
///
/// # Examples
///
/// ```ignore
/// use kj::failed;
///
/// fn parse(input: &str) -> kj::Result<u32> {
///     input.parse().map_err(|_| failed!("invalid number: '{}'", input))
/// }
/// ```
#[macro_export]
macro_rules! failed {
    ($msg:literal $(, $arg:expr)* $(,)?) => {
        $crate::KjError::new($crate::KjExceptionType::Failed, format!($msg $(, $arg)*))
    };
}

/// Builds a [`cxx::KjError`] with [`cxx::KjExceptionType::Overloaded`] and a `format!`-style
/// message.
///
/// This is the Rust equivalent of the C++ `KJ_EXCEPTION(OVERLOADED, ...)` macro. Use this for
/// calls that failed because of a temporary lack of resources (retryable). See [`failed!`] for
/// general usage notes.
#[macro_export]
macro_rules! overloaded {
    ($msg:literal $(, $arg:expr)* $(,)?) => {
        $crate::KjError::new($crate::KjExceptionType::Overloaded, format!($msg $(, $arg)*))
    };
}

/// Builds a [`cxx::KjError`] with [`cxx::KjExceptionType::Disconnected`] and a `format!`-style
/// message.
///
/// This is the Rust equivalent of the C++ `KJ_EXCEPTION(DISCONNECTED, ...)` macro. Use this when
/// the call required communication over a connection that has been lost. See [`failed!`] for
/// general usage notes.
#[macro_export]
macro_rules! disconnected {
    ($msg:literal $(, $arg:expr)* $(,)?) => {
        $crate::KjError::new($crate::KjExceptionType::Disconnected, format!($msg $(, $arg)*))
    };
}

/// Builds a [`cxx::KjError`] with [`cxx::KjExceptionType::Unimplemented`] and a `format!`-style
/// message.
///
/// This is the Rust equivalent of the C++ `KJ_UNIMPLEMENTED(...)` / `KJ_EXCEPTION(UNIMPLEMENTED,
/// ...)` macros. Named `not_implemented!` rather than `unimplemented!` to avoid shadowing
/// `std::unimplemented!` at import sites. See [`failed!`] for general usage notes.
#[macro_export]
macro_rules! not_implemented {
    ($msg:literal $(, $arg:expr)* $(,)?) => {
        $crate::KjError::new($crate::KjExceptionType::Unimplemented, format!($msg $(, $arg)*))
    };
}

/// Unconditionally returns `Err(...)` from the enclosing function with a
/// [`cxx::KjExceptionType::Failed`] error.
///
/// This is the Rust equivalent of the C++ `KJ_FAIL_REQUIRE(...)` macro. The enclosing function
/// must return `kj::Result<T>` (or any `Result<T, E>` where `E: From<cxx::KjError>`).
///
/// # Examples
///
/// ```ignore
/// use kj::fail_require;
///
/// fn require_positive(n: i32) -> kj::Result<u32> {
///     if n <= 0 {
///         fail_require!("expected a positive number, got {}", n);
///     }
///     Ok(n as u32)
/// }
/// ```
#[macro_export]
macro_rules! fail_require {
    ($msg:literal $(, $arg:expr)* $(,)?) => {
        return Err($crate::failed!($msg $(, $arg)*).into())
    };
}

/// Unconditionally returns `Err(...)` from the enclosing function with a
/// [`cxx::KjExceptionType::Overloaded`] error.
///
/// This is the Rust equivalent of the C++ `KJ_EXCEPTION(OVERLOADED, ...)` thrown via
/// `KJ_FAIL_REQUIRE`-style control flow. See [`fail_require!`] for general usage notes.
#[macro_export]
macro_rules! overloaded_require {
    ($msg:literal $(, $arg:expr)* $(,)?) => {
        return Err($crate::overloaded!($msg $(, $arg)*).into())
    };
}

/// Unconditionally returns `Err(...)` from the enclosing function with a
/// [`cxx::KjExceptionType::Disconnected`] error.
///
/// This is the Rust equivalent of the C++ `KJ_EXCEPTION(DISCONNECTED, ...)` thrown via
/// `KJ_FAIL_REQUIRE`-style control flow. See [`fail_require!`] for general usage notes.
#[macro_export]
macro_rules! disconnected_require {
    ($msg:literal $(, $arg:expr)* $(,)?) => {
        return Err($crate::disconnected!($msg $(, $arg)*).into())
    };
}

/// Unconditionally returns `Err(...)` from the enclosing function with a
/// [`cxx::KjExceptionType::Unimplemented`] error.
///
/// This is the Rust equivalent of the C++ `KJ_UNIMPLEMENTED(...)` macro. See [`fail_require!`]
/// for general usage notes.
///
/// # Examples
///
/// ```ignore
/// use kj::not_implemented_require;
///
/// fn connect() -> kj::Result<()> {
///     not_implemented_require!("connect is not supported by this worker");
/// }
/// ```
#[macro_export]
macro_rules! not_implemented_require {
    ($msg:literal $(, $arg:expr)* $(,)?) => {
        return Err($crate::not_implemented!($msg $(, $arg)*).into())
    };
}
