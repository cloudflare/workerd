// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/// Generates a no-op [`Traced`](crate::Traced) implementation for types with no GC-visible
/// references.
///
/// Types that contain only plain data (no `jsg::Rc`, `jsg::v8::Global`, etc.) do not
/// need to participate in GC tracing. This macro produces an empty `Traced` impl
/// whose `trace` method is a no-op.
///
/// # Example
///
/// ```ignore
/// use jsg::jsg_traced;
///
/// struct Id(u64);
/// jsg_traced!(Id);
/// ```
#[macro_export]
macro_rules! jsg_traced {
    ($($t:ty),* $(,)?) => {
        $(impl $crate::Traced for $t {})*
    };
}

/// Validates a condition at runtime, returning a [`jsg::Error`](crate::Error) if the condition
/// is `false`.
///
/// This is the Rust equivalent of the C++ `JSG_REQUIRE` macro defined in
/// `src/workerd/jsg/exception.h`. While the C++ version throws a KJ exception, this macro
/// returns `Err(jsg::Error)` via the `?` operator, following Rust's error-handling conventions.
///
/// # Syntax
///
/// ```ignore
/// jsg_require!(condition, ExceptionVariant, "message");
/// jsg_require!(condition, ExceptionVariant, "format {} string", arg1, arg2);
/// ```
///
/// # Parameters
///
/// - `condition` — Any expression that evaluates to `bool`. When `true`, the macro is a no-op.
///   When `false`, an error is returned.
/// - `ExceptionVariant` — One of the [`ExceptionType`](crate::v8::ffi::ExceptionType) variants
///   that determines the JavaScript error class thrown to user code. Available variants:
///   `TypeError`, `RangeError`, `ReferenceError`, `SyntaxError`, `Error`,
///   `OperationError`, `DataError`, `DataCloneError`, `InvalidAccessError`,
///   `InvalidStateError`, `InvalidCharacterError`, `NotSupportedError`,
///   `TimeoutError`, `TypeMismatchError`, `AbortError`, `NotFoundError`.
/// - `"message"` / `"format string", args...` — A message string, optionally with `format!`-style
///   arguments. Unlike the C++ `JSG_REQUIRE` which concatenates via `kj::str()`, this macro uses
///   Rust's standard `format!()` for string interpolation.
///
/// # Return Type
///
/// The enclosing function must return `jsg::Result<T>` (or any `Result<T, E>` where
/// `E: From<jsg::Error>`). The macro expands to an early `return Err(...)` on failure.
///
/// # Examples
///
/// ```ignore
/// use jsg::{jsg_require, Result};
///
/// fn parse_port(value: u32) -> Result<u16> {
///     jsg_require!(value <= 65535, RangeError, "port {} out of range", value);
///     Ok(value as u16)
/// }
///
/// fn require_non_empty(s: &str) -> Result<()> {
///     jsg_require!(!s.is_empty(), TypeError, "string must not be empty");
///     Ok(())
/// }
/// ```
///
/// # Comparison with C++
///
/// | C++ | Rust |
/// |-----|------|
/// | `JSG_REQUIRE(port <= 65535, RangeError, "port ", port, " out of range")` | `jsg_require!(port <= 65535, RangeError, "port {} out of range", port)` |
/// | Throws `kj::Exception` | Returns `Err(jsg::Error)` |
/// | Uses `kj::str()` concatenation | Uses `format!()` interpolation |
#[macro_export]
macro_rules! jsg_require {
    ($cond:expr, $err_type:ident, $msg:literal $(, $arg:expr)* $(,)?) => {
        if !($cond) {
            return Err($crate::Error {
                name: $crate::ExceptionType::$err_type,
                message: format!($msg $(, $arg)*),
            });
        }
    };
}

/// Unconditionally returns a [`jsg::Error`](crate::Error) from the enclosing function.
///
/// This is the Rust equivalent of the C++ `JSG_FAIL_REQUIRE` macro defined in
/// `src/workerd/jsg/exception.h`. While the C++ version throws a KJ exception
/// unconditionally, this macro returns `Err(jsg::Error)` via an early `return`,
/// following Rust's error-handling conventions.
///
/// # Syntax
///
/// ```ignore
/// jsg_fail_require!(ExceptionVariant, "message");
/// jsg_fail_require!(ExceptionVariant, "format {} string", arg1, arg2);
/// ```
///
/// # Parameters
///
/// - `ExceptionVariant` — One of the [`ExceptionType`](crate::v8::ffi::ExceptionType) variants
///   that determines the JavaScript error class thrown to user code. Available variants:
///   `TypeError`, `RangeError`, `ReferenceError`, `SyntaxError`, `Error`,
///   `OperationError`, `DataError`, `DataCloneError`, `InvalidAccessError`,
///   `InvalidStateError`, `InvalidCharacterError`, `NotSupportedError`,
///   `TimeoutError`, `TypeMismatchError`, `AbortError`, `NotFoundError`.
/// - `"message"` / `"format string", args...` — A message string, optionally with `format!`-style
///   arguments. Unlike the C++ `JSG_FAIL_REQUIRE` which concatenates via `kj::str()`, this macro
///   uses Rust's standard `format!()` for string interpolation.
///
/// # Return Type
///
/// The enclosing function must return `jsg::Result<T>` (or any `Result<T, E>` where
/// `E: From<jsg::Error>`). The macro always triggers an early `return Err(...)`.
///
/// # Examples
///
/// ```ignore
/// use jsg::{jsg_fail_require, Result};
///
/// fn unsupported_algorithm(name: &str) -> Result<()> {
///     jsg_fail_require!(NotSupportedError, "algorithm '{}' is not supported", name);
/// }
///
/// fn validate_input(mode: &str) -> Result<String> {
///     match mode {
///         "fast" => Ok("fast-path".to_owned()),
///         "slow" => Ok("slow-path".to_owned()),
///         _ => jsg_fail_require!(TypeError, "invalid mode: '{}'", mode),
///     }
/// }
/// ```
///
/// # Comparison with C++
///
/// | C++ | Rust |
/// |-----|------|
/// | `JSG_FAIL_REQUIRE(TypeError, "invalid mode: ", mode)` | `jsg_fail_require!(TypeError, "invalid mode: '{}'", mode)` |
/// | Throws `kj::Exception` | Returns `Err(jsg::Error)` |
/// | Uses `kj::str()` concatenation | Uses `format!()` interpolation |
#[macro_export]
macro_rules! jsg_fail_require {
    ($err_type:ident, $msg:literal $(, $arg:expr)* $(,)?) => {
        return Err($crate::Error {
            name: $crate::ExceptionType::$err_type,
            message: format!($msg $(, $arg)*),
        })
    };
}
