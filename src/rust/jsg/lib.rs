// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::future::Future;
use std::num::ParseIntError;
use std::ops::Deref;

pub mod feature_flags;
pub mod function;
pub mod macros;
pub mod modules;
pub mod nullable;
pub mod resource;
pub mod v8;
mod wrappable;

pub use feature_flags::FeatureFlags;
pub use function::Function;
pub use function::FunctionArgs;
pub use nullable::Nullable;
pub use resource::Rc;
pub use resource::Resource;
pub use resource::Weak;
pub use v8::ArrayBuffer;
pub use v8::ArrayBufferView;
pub use v8::BackingStore;
pub use v8::BigInt64Array;
pub use v8::BigUint64Array;
pub use v8::Float32Array;
pub use v8::Float64Array;
pub use v8::GcVisitor;
pub use v8::Int8Array;
pub use v8::Int16Array;
pub use v8::Int32Array;
pub use v8::IsolatePtr;
pub use v8::Uint8Array;
pub use v8::Uint16Array;
pub use v8::Uint32Array;
pub use v8::ffi::ExceptionType;
pub use wrappable::FromJS;
pub use wrappable::ToJS;
pub use wrappable::Traced;

#[cxx::bridge(namespace = "workerd::rust::jsg")]
mod ffi {
    extern "Rust" {
        type Realm;

        /// Create a fully-initialized Realm with feature flags.
        /// `feature_flags_data` is canonical (single-segment, no segment table) Cap'n Proto
        /// bytes produced by `capnp::canonicalize()` on the C++ side.
        #[expect(clippy::unnecessary_box_returns)]
        unsafe fn realm_create(isolate: *mut Isolate, feature_flags_data: &[u8]) -> Box<Realm>;
    }

    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type Isolate = crate::v8::ffi::Isolate;

        // Realm
        pub unsafe fn realm_from_isolate(isolate: *mut Isolate) -> *mut Realm;
    }
}

pub type Result<T, E = Error> = std::result::Result<T, E>;

impl From<&str> for ExceptionType {
    fn from(value: &str) -> Self {
        match value {
            "OperationError" => Self::OperationError,
            "DataError" => Self::DataError,
            "DataCloneError" => Self::DataCloneError,
            "InvalidAccessError" => Self::InvalidAccessError,
            "InvalidStateError" => Self::InvalidStateError,
            "InvalidCharacterError" => Self::InvalidCharacterError,
            "NotSupportedError" => Self::NotSupportedError,
            "SyntaxError" => Self::SyntaxError,
            "TimeoutError" => Self::TimeoutError,
            "TypeMismatchError" => Self::TypeMismatchError,
            "AbortError" => Self::AbortError,
            "NotFoundError" => Self::NotFoundError,
            "TypeError" => Self::TypeError,
            "RangeError" => Self::RangeError,
            "ReferenceError" => Self::ReferenceError,
            _ => Self::Error,
        }
    }
}

#[derive(Debug, Clone)]
pub struct Error {
    pub name: ExceptionType,
    pub message: String,
    /// If `true`, `message` must never reach guest JS verbatim: it may contain
    /// implementation details (a raw KJ exception description, a C++ source
    /// expression, etc.). `Lock::throw_exception()` redacts it, throwing a
    /// generic `"internal error; reference = <id>"` instead and logging
    /// `message` (with that ID) via `KJ_LOG(ERROR)`. Mirrors the `isInternal`
    /// flag computed by C++ `jsg::tunneledErrorType()` / applied by
    /// `decodeTunneledException()` (`src/workerd/jsg/exception.c++`,
    /// `src/workerd/jsg/util.c++`). Constructors in this module (and the
    /// `impl_error_constructors!` macro) always set this to `false`; only
    /// `from_kj_description()` sets it to `true`.
    is_internal: bool,
    /// If `true`, this error represents isolate termination surfaced across the
    /// FFI (the C++ shims' `TERMINATED_DESCRIPTION` tunnel, see
    /// `src/rust/jsg/ffi.c++`), not a catchable JS exception.
    /// `Lock::throw_exception()` re-raises it by re-arming
    /// `terminate_execution()` instead of scheduling a JS throw. Only
    /// `from_kj_description()` sets it; guest JS cannot forge it because user
    /// exceptions tunnel under the `jsg.` prefix, not `jsg-internal.`.
    is_termination: bool,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.name, self.message)
    }
}

/// Generates constructor methods for each `ExceptionType` variant.
/// e.g., `new_type_error("message")` creates an Error with `ExceptionType::TypeError`
macro_rules! impl_error_constructors {
    ($($variant:ident => $fn_name:ident),* $(,)?) => {
        impl Error {
            $(
                pub fn $fn_name(message: impl Into<String>) -> Self {
                    Self {
                        name: ExceptionType::$variant,
                        message: message.into(),
                        is_internal: false,
                        is_termination: false,
                    }
                }
            )*
        }
    };
}

impl_error_constructors! {
    OperationError => new_operation_error,
    DataError => new_data_error,
    DataCloneError => new_data_clone_error,
    InvalidAccessError => new_invalid_access_error,
    InvalidStateError => new_invalid_state_error,
    InvalidCharacterError => new_invalid_character_error,
    NotSupportedError => new_not_supported_error,
    SyntaxError => new_syntax_error,
    TimeoutError => new_timeout_error,
    TypeMismatchError => new_type_mismatch_error,
    AbortError => new_abort_error,
    NotFoundError => new_not_found_error,
    TypeError => new_type_error,
    Error => new_error,
    RangeError => new_range_error,
    ReferenceError => new_reference_error,
}

impl FromJS for Error {
    type ResultType = Self;

    /// Creates an Error from a V8 value (typically an exception).
    ///
    /// If the value is a native error, extracts the name and message properties.
    /// Otherwise, converts the value to a string for the message.
    fn from_js(lock: &mut Lock, value: v8::Local<v8::Value>) -> Result<Self::ResultType, Error> {
        if value.is_native_error() {
            let obj: v8::Local<v8::Object> = value.into();

            let name = obj
                .get(lock, "name")
                .and_then(|v| String::from_js(lock, v).ok());

            let message = obj
                .get(lock, "message")
                .and_then(|v| String::from_js(lock, v).ok())
                .unwrap_or_else(|| "Unknown error".to_owned());

            Ok(Self {
                name: name.map_or(ExceptionType::Error, |n| ExceptionType::from(n.as_str())),
                message,
                is_internal: false,
                is_termination: false,
            })
        } else {
            Err(Self::new_type_error("Unknown error"))
        }
    }
}

impl Error {
    pub fn new(name: &str, message: &str) -> Self {
        Self {
            name: ExceptionType::from(name),
            message: message.to_owned(),
            is_internal: false,
            is_termination: false,
        }
    }

    /// Constructs a non-internal `Error` with an explicit [`ExceptionType`].
    ///
    /// Used by the `jsg_require!`/`jsg_fail_require!` macros (`macros.rs`), which are
    /// given an `ExceptionType` variant as a bare identifier and so can't call the
    /// per-variant `new_*_error()` constructors generated by
    /// `impl_error_constructors!` without a `snake_case` name for it. Not meant for
    /// general use outside this crate; prefer the `new_*_error()` constructors.
    #[doc(hidden)]
    pub fn from_type(name: ExceptionType, message: impl Into<String>) -> Self {
        Self {
            name,
            message: message.into(),
            is_internal: false,
            is_termination: false,
        }
    }

    /// Creates a V8 exception from this error.
    ///
    /// This always builds the exception from `self.message` verbatim: it does
    /// not check `is_internal` / redact anything. Prefer `Lock::throw_exception()`,
    /// which does perform that check before falling back to this method.
    pub fn to_local<'a>(&self, isolate: v8::IsolatePtr) -> v8::Local<'a, v8::Value> {
        let message = self.message.as_bytes();
        // Strings longer than i32::MAX bytes are truncated to satisfy V8's
        // int-typed length parameter; V8 rejects strings that large anyway.
        let len = i32::try_from(message.len()).unwrap_or(i32::MAX);
        // SAFETY: isolate is valid and locked (guaranteed by caller); message
        // pointer and len describe a valid byte slice.
        unsafe {
            v8::Local::from_ffi(
                isolate,
                v8::ffi::exception_create_from_bytes(
                    isolate.as_ffi(),
                    self.name,
                    message.as_ptr(),
                    len,
                ),
            )
        }
    }
}

impl From<ParseIntError> for Error {
    fn from(err: ParseIntError) -> Self {
        Self::new_range_error(format!("Failed to parse integer: {err}"))
    }
}

impl From<cxx::KjException> for Error {
    /// Converts a `kj::Exception` caught at the FFI boundary into a JS-throwable
    /// [`Error`], parsing the `jsg.<Type>: <message>` tunneling prefix.
    fn from(err: cxx::KjException) -> Self {
        Self::from_kj_description(err.what())
    }
}

impl Error {
    /// Delimiter C++ `KJ_REQUIRE`/`KJ_ASSERT` insert between a failed condition (or
    /// `annotateBroken()` reason) and the actual message.
    const ERROR_PREFIX_DELIM: &'static str = "; ";
    /// Repeated when an error is tunneled over RPC (see `annotateBroken()`).
    const ERROR_REMOTE_PREFIX: &'static str = "remote.";

    /// Parses a KJ exception description for a JSG tunneling prefix, mirroring the
    /// prefix-peeling of C++ `jsg::tunneledErrorType()`
    /// (`src/workerd/jsg/exception.c++`). Unrecognized descriptions are treated as
    /// internal, matching C++'s `makeDefaultError()`: the returned `Error` has
    /// `is_internal` set, so `Lock::throw_exception()` redacts `message` rather than
    /// exposing it to guest JS.
    ///
    /// This mirrors only the *prefix-peeling structure* of `tunneledErrorType()`, not
    /// its full semantics: `isFromRemote` / `isDurableObjectReset` /
    /// `isDoNotLogException` are peeled off (to keep peeling in sync with C++) but the
    /// resulting signals are discarded rather than attached to the `Error`.
    ///
    /// A tunneled JSG error is `jsg.<Type>: <message>` or
    /// `jsg.DOMException(<Name>): <message>` (or `jsg-internal.` instead of `jsg.`,
    /// meaning "throw this JS error type, but still redact the message text"),
    /// anchored at the start of the description -- but the description is very often
    /// wrapped first. In particular, `JSG_REQUIRE`/`JSG_ASSERT` (the common way JSG
    /// code throws tunneled errors) go through `KJ_REQUIRE`/`KJ_ASSERT` respectively,
    /// both of which prepend `expected <cond>; ` ahead of the message. Failing to peel
    /// that (and the other wrapper prefixes below) off first means the tunneling tag is
    /// never found and every such error gets rethrown as a generic, untyped error
    /// instead of its real JS error type.
    fn from_kj_description(description: &str) -> Self {
        let mut msg = description;

        // The error may have been tunneled over RPC one or more times.
        while let Some(rest) = msg.strip_prefix("remote exception: ") {
            msg = rest;
        }
        while let Some(rest) = msg.strip_prefix(Self::ERROR_REMOTE_PREFIX) {
            msg = rest;
        }

        if msg.starts_with("expected ") {
            // A failed KJ_REQUIRE/KJ_ASSERT condition. Peel away "<cond>; " segments
            // (there can be more than one, e.g. from nested requires) until we find a
            // tunneled error or run out of delimiters.
            let mut rest = msg;
            while let Some(idx) = rest.find(Self::ERROR_PREFIX_DELIM) {
                rest = &rest[idx + Self::ERROR_PREFIX_DELIM.len()..];
                if let Some(err) = Self::try_extract_tunneled(rest) {
                    return err;
                }
            }
            // No tunneling tag found: matches C++ `makeDefaultError()`, which marks
            // the message `isInternal` and redacts it before it reaches guest JS.
            return Self::new_internal_error(description.to_owned());
        }

        // Trim any number of "broken.<reason>; " prefixes (from `annotateBroken()`).
        while msg.starts_with("broken.") {
            match msg.find(Self::ERROR_PREFIX_DELIM) {
                Some(idx) => msg = &msg[idx + Self::ERROR_PREFIX_DELIM.len()..],
                None => break,
            }
        }

        Self::try_extract_tunneled(msg)
            .unwrap_or_else(|| Self::new_internal_error(description.to_owned()))
    }

    /// Tries to parse `msg` as a `jsg.<Type>: <message>` or
    /// `jsg.DOMException(<Name>): <message>` tunneled error body (also accepting the
    /// `jsg-internal.` prefix, which marks the resulting `Error` as `is_internal`, so
    /// its message is redacted even though its JS error type is preserved -- mirroring
    /// C++'s `ERROR_INTERNAL_SOURCE_PREFIX_JSG` handling). Returns `None` if `msg`
    /// doesn't start with a recognized tunneling tag.
    fn try_extract_tunneled(msg: &str) -> Option<Self> {
        let (body, is_internal) = match msg.strip_prefix("jsg.") {
            Some(body) => (body, false),
            None => (msg.strip_prefix("jsg-internal.")?, true),
        };

        // Isolate termination tunneled by the FFI shims (TERMINATED_DESCRIPTION in
        // ffi.c++). Only recognized under the unforgeable `jsg-internal.` prefix.
        if is_internal && let Some(message) = body.strip_prefix("Terminated: ") {
            return Some(Self {
                name: ExceptionType::Error,
                message: message.to_owned(),
                is_internal: true,
                is_termination: true,
            });
        }

        if let Some(rest) = body.strip_prefix("DOMException(")
            && let Some((name, message)) = rest.split_once("): ")
        {
            return Some(Self {
                name: ExceptionType::from(name),
                message: message.to_owned(),
                is_internal,
                is_termination: false,
            });
        }

        let (ty, message) = body.split_once(": ")?;
        Some(Self {
            name: ExceptionType::from(ty),
            message: message.to_owned(),
            is_internal,
            is_termination: false,
        })
    }

    /// Returns `true` if this error represents isolate termination rather than a
    /// catchable JS exception.
    ///
    /// Callers looping over JS callbacks should treat this as a signal to stop
    /// immediately: the termination flag is pending on the isolate, so further JS
    /// entry only fails again. Throwing the error via `Lock::throw_exception()`
    /// re-arms termination rather than scheduling a JS throw.
    pub fn is_termination(&self) -> bool {
        self.is_termination
    }

    /// Like `new_type_error()`, but marks the resulting `Error` `is_internal`, so
    /// `Lock::throw_exception()` redacts `message` instead of exposing it to guest JS.
    /// The JS-visible error type ends up as plain `Error` regardless of the type set
    /// here (`Lock::throw_internal_error()` -> C++ `makeInternalError()` always throws
    /// a plain `Error`), so `name` only matters if something inspects the `Error`
    /// value directly without throwing it (e.g. in tests).
    fn new_internal_error(message: impl Into<String>) -> Self {
        Self {
            name: ExceptionType::TypeError,
            message: message.into(),
            is_internal: true,
            is_termination: false,
        }
    }
}

#[cfg(test)]
mod tunneled_error_tests {
    use super::*;

    fn from_description(description: &str) -> Error {
        Error::from_kj_description(description)
    }

    #[test]
    fn plain_jsg_prefix() {
        let err = from_description("jsg.TypeError: boom");
        assert_eq!(err.name, ExceptionType::TypeError);
        assert_eq!(err.message, "boom");
        assert!(!err.is_internal, "jsg. errors must not be redacted");
    }

    #[test]
    fn termination_tunnel() {
        let err = from_description("jsg-internal.Terminated: JavaScript execution terminated");
        assert!(err.is_termination());
        assert!(err.is_internal);
        assert_eq!(err.message, "JavaScript execution terminated");
    }

    #[test]
    fn termination_not_forgeable_from_guest_prefix() {
        // A user error named "Terminated" tunnels under `jsg.`, which must not
        // be treated as termination.
        let err = from_description("jsg.Terminated: fake");
        assert!(!err.is_termination());
        assert!(!err.is_internal);
    }

    #[test]
    fn jsg_require_wraps_with_expected_prefix() {
        // What JSG_REQUIRE(cond, TypeError, "boom") actually produces via KJ_REQUIRE.
        let err = from_description("expected someCondition; jsg.TypeError: boom");
        assert_eq!(err.name, ExceptionType::TypeError);
        assert_eq!(err.message, "boom");
        assert!(!err.is_internal);
    }

    #[test]
    fn nested_expected_prefixes() {
        let err = from_description("expected a; expected b; jsg.RangeError: boom");
        assert_eq!(err.name, ExceptionType::RangeError);
        assert_eq!(err.message, "boom");
        assert!(!err.is_internal);
    }

    #[test]
    fn broken_prefix() {
        let err = from_description("broken.inputGateBroken; jsg.Error: boom");
        assert_eq!(err.name, ExceptionType::Error);
        assert_eq!(err.message, "boom");
        assert!(!err.is_internal);
    }

    #[test]
    fn remote_prefix() {
        let err = from_description("remote.jsg.AbortError: boom");
        assert_eq!(err.name, ExceptionType::AbortError);
        assert_eq!(err.message, "boom");
        assert!(!err.is_internal);
    }

    #[test]
    fn remote_exception_prefix() {
        let err = from_description("remote exception: jsg.TypeError: boom");
        assert_eq!(err.name, ExceptionType::TypeError);
        assert_eq!(err.message, "boom");
        assert!(!err.is_internal);
    }

    #[test]
    fn dom_exception() {
        let err = from_description("jsg.DOMException(AbortError): boom");
        assert_eq!(err.name, ExceptionType::AbortError);
        assert_eq!(err.message, "boom");
        assert!(!err.is_internal);
    }

    #[test]
    fn dom_exception_with_expected_prefix() {
        let err = from_description("expected someCondition; jsg.DOMException(AbortError): boom");
        assert_eq!(err.name, ExceptionType::AbortError);
        assert_eq!(err.message, "boom");
        assert!(!err.is_internal);
    }

    #[test]
    fn jsg_internal_prefix_preserves_type_but_is_redacted() {
        let err = from_description("jsg-internal.TypeError: boom");
        assert_eq!(err.name, ExceptionType::TypeError);
        assert_eq!(err.message, "boom");
        assert!(
            err.is_internal,
            "jsg-internal. errors must be redacted by Lock::throw_exception()"
        );
    }

    #[test]
    fn jsg_internal_dom_exception_is_redacted() {
        let err = from_description("jsg-internal.DOMException(OperationError): boom");
        assert_eq!(err.name, ExceptionType::OperationError);
        assert_eq!(err.message, "boom");
        assert!(err.is_internal);
    }

    #[test]
    fn unrecognized_description_is_redacted() {
        let err = from_description("some unrelated kj exception");
        assert_eq!(err.message, "some unrelated kj exception");
        assert!(
            err.is_internal,
            "untunneled descriptions must be redacted, matching C++ makeDefaultError()"
        );
    }

    #[test]
    fn expected_prefix_without_tunneling_tag_is_redacted() {
        let err = from_description("expected someCondition; some message");
        assert_eq!(err.message, "expected someCondition; some message");
        assert!(err.is_internal);
    }
}

/// A wrapper type that prevents automatic type coercion when unwrapping from JavaScript.
///
/// JavaScript automatically coerces types in certain contexts. For instance, when a JavaScript
/// API expects a string, calling it with the value `null` will result in the null being coerced
/// into the string value `"null"`.
///
/// `NonCoercible<T>` can be used to disable automatic type coercion in APIs. For instance,
/// `NonCoercible<String>` can be used to accept a value only if the input is already a string.
/// If the input is the value `null`, then an error is thrown rather than silently coercing to
/// `"null"`.
///
/// # Supported Types
///
/// Any type implementing the [`Type`] trait can be used with `NonCoercible<T>`. Built-in
/// implementations include:
///
/// - `NonCoercible<String>` - only accepts JavaScript strings
/// - `NonCoercible<bool>` - only accepts JavaScript booleans
/// - `NonCoercible<Number>` - only accepts JavaScript numbers
///
/// # Example
///
/// ```ignore
/// use jsg::NonCoercible;
///
/// // This function will only accept actual strings, not values that can be coerced to strings
/// #[jsg_method]
/// pub fn process_string(&self, param: NonCoercible<String>) -> Result<(), Error> {
///     let s: &String = param.as_ref();
///     // or use Deref: let s: &str = &*param;
///     // ...
/// }
/// ```
///
/// # Important Notes
///
/// Using `NonCoercible<T>` runs counter to Web IDL and general JavaScript API conventions.
/// In nearly all cases, APIs should allow coercion to occur and should deal with the coerced
/// input accordingly to avoid being a source of user confusion. Only use `NonCoercible` if
/// you have a good reason to disable coercion.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NonCoercible<T> {
    value: T,
}

impl<T> NonCoercible<T> {
    /// Creates a new `NonCoercible` wrapper around the given value.
    pub fn new(value: T) -> Self {
        Self { value }
    }

    /// Consumes the wrapper and returns the inner value.
    pub fn into_inner(self) -> T {
        self.value
    }
}

impl<T> From<T> for NonCoercible<T> {
    fn from(value: T) -> Self {
        Self::new(value)
    }
}

impl<T> AsRef<T> for NonCoercible<T> {
    fn as_ref(&self) -> &T {
        &self.value
    }
}

impl<T> Deref for NonCoercible<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        &self.value
    }
}

/// A wrapper type for JavaScript numbers (IEEE 754 double-precision floats).
///
/// `Number` represents JavaScript's `number` type, which is always a 64-bit
/// floating-point value. This wrapper type is used instead of raw `f64` to
/// distinguish between JavaScript numbers and Rust's `f64` type used for
/// `Float64Array` elements.
///
/// # Usage
///
/// Use `Number` when you need to accept or return JavaScript numbers in your API:
///
/// ```ignore
/// use jsg::Number;
///
/// #[jsg_method]
/// pub fn add(&self, a: Number, b: Number) -> Number {
///     Number::new(a.value() + b.value())
/// }
/// ```
///
/// # Type Mapping
///
/// | Rust Type | JavaScript Type |
/// |-----------|-----------------|
/// | `jsg::Number` | `number` |
/// | `f64` | Used for `Float64Array` elements |
/// | `Vec<f64>` | `Float64Array` |
#[derive(Debug, Clone, Copy, PartialEq, PartialOrd, Default)]
pub struct Number {
    value: f64,
}

impl Number {
    /// The largest integer that can be represented exactly in JavaScript (2^53 - 1).
    ///
    /// [MDN documentation](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Number/MAX_SAFE_INTEGER)
    pub const MAX_SAFE_INTEGER: f64 = 9_007_199_254_740_991.0; // 2^53 - 1

    /// The smallest integer that can be represented exactly in JavaScript (-(2^53 - 1)).
    ///
    /// [MDN documentation](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Number/MIN_SAFE_INTEGER)
    pub const MIN_SAFE_INTEGER: f64 = -9_007_199_254_740_991.0; // -(2^53 - 1)

    /// Creates a new `Number` from an `f64` value.
    #[inline]
    pub fn new(value: f64) -> Self {
        Self { value }
    }

    /// Returns the underlying `f64` value.
    #[inline]
    pub fn value(&self) -> f64 {
        self.value
    }

    /// Consumes the wrapper and returns the inner `f64` value.
    #[inline]
    pub fn into_inner(self) -> f64 {
        self.value
    }

    /// Determines whether the value is a finite number.
    ///
    /// Returns `true` if the value is finite (not `Infinity`, `-Infinity`, or `NaN`).
    ///
    /// [MDN documentation](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Number/isFinite)
    #[inline]
    pub fn is_finite(&self) -> bool {
        self.value.is_finite()
    }

    /// Determines whether the value is an integer.
    ///
    /// Returns `true` if the value is finite and has no fractional part.
    ///
    /// [MDN documentation](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Number/isInteger)
    #[inline]
    #[expect(clippy::float_cmp)] // Exact comparison is correct here - we want trunc(x) == x
    pub fn is_integer(&self) -> bool {
        self.value.is_finite() && self.value.trunc() == self.value
    }

    /// Determines whether the value is `NaN`.
    ///
    /// This is more robust than the global `isNaN()` because it doesn't coerce
    /// the value to a number first.
    ///
    /// [MDN documentation](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Number/isNaN)
    #[inline]
    pub fn is_nan(&self) -> bool {
        self.value.is_nan()
    }

    /// Determines whether the value is a safe integer.
    ///
    /// A safe integer is an integer that:
    /// - Can be exactly represented as an IEEE-754 double precision number
    /// - Has an IEEE-754 representation that cannot be the result of rounding any other integer
    ///
    /// Safe integers range from -(2^53 - 1) to 2^53 - 1, inclusive.
    ///
    /// [MDN documentation](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Number/isSafeInteger)
    #[inline]
    pub fn is_safe_integer(&self) -> bool {
        self.is_integer()
            && self.value >= Self::MIN_SAFE_INTEGER
            && self.value <= Self::MAX_SAFE_INTEGER
    }
}

impl From<f64> for Number {
    fn from(value: f64) -> Self {
        Self::new(value)
    }
}

impl From<Number> for f64 {
    fn from(num: Number) -> Self {
        num.value
    }
}

impl From<i32> for Number {
    fn from(value: i32) -> Self {
        Self::new(f64::from(value))
    }
}

impl From<u32> for Number {
    fn from(value: u32) -> Self {
        Self::new(f64::from(value))
    }
}

impl std::fmt::Display for Number {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.value)
    }
}

/// Proof that the V8 isolate is valid and exclusively locked by the current thread.
///
/// A `Lock` instance can only be created when the C++ `jsg::Lock` (or equivalent)
/// is held, meaning:
/// - The `v8::Isolate` pointer is valid and has not been disposed.
/// - The current thread holds the isolate lock (`v8::Locker` is active).
/// - No other thread can enter the isolate concurrently.
///
/// `Lock` is passed to resource methods and callbacks to perform V8 operations
/// like creating objects, wrapping values, and accessing the `Realm`. It is
/// analogous to `jsg::Lock&` in C++ JSG.
///
/// `Lock` is neither `Send` nor `Sync` — it cannot escape the thread that
/// created it.
pub struct Lock {
    isolate: v8::IsolatePtr,
}

impl Lock {
    /// # Safety
    /// The caller must ensure that `args` is a valid pointer to `FunctionCallbackInfo`.
    pub unsafe fn from_args(args: *mut v8::ffi::FunctionCallbackInfo) -> Self {
        // SAFETY: args is a valid FunctionCallbackInfo pointer (guaranteed by caller).
        unsafe { Self::from_isolate_ptr(v8::ffi::fci_get_isolate(args)) }
    }

    /// Creates a Lock from a raw isolate pointer.
    ///
    /// # Safety
    /// The caller must ensure that `isolate` is a valid pointer to an `Isolate`.
    ///
    /// # Panics
    /// Panics if the isolate is not currently locked.
    pub unsafe fn from_isolate_ptr(isolate: *mut v8::ffi::Isolate) -> Self {
        // SAFETY: isolate pointer is valid (guaranteed by caller).
        let isolate = unsafe { v8::IsolatePtr::from_ffi(isolate) };
        // SAFETY: Lock guarantees the isolate is valid
        assert!(unsafe { isolate.is_locked() });
        Self { isolate }
    }

    /// Returns the isolate associated with this lock.
    pub fn isolate(&self) -> v8::IsolatePtr {
        self.isolate
    }

    pub fn new_object<'a>(&mut self) -> v8::Local<'a, v8::Object> {
        // SAFETY: Lock guarantees the isolate is valid and locked
        unsafe {
            v8::Local::from_ffi(
                self.isolate(),
                v8::ffi::local_new_object(self.isolate().as_ffi()),
            )
        }
    }

    /// Creates a JavaScript `Error` object with the given message.
    ///
    /// Mirrors C++ `jsg::Lock::error()`. The returned object can have additional
    /// properties attached before being thrown or returned.
    pub fn error<'a>(&mut self, message: &str) -> v8::Local<'a, v8::Object> {
        self.create_error(ExceptionType::Error, message)
    }

    /// Creates a JavaScript `TypeError` object with the given message.
    ///
    /// Mirrors C++ `jsg::Lock::typeError()`.
    pub fn type_error<'a>(&mut self, message: &str) -> v8::Local<'a, v8::Object> {
        self.create_error(ExceptionType::TypeError, message)
    }

    /// Creates a JavaScript `RangeError` object with the given message.
    ///
    /// Mirrors C++ `jsg::Lock::rangeError()`.
    pub fn range_error<'a>(&mut self, message: &str) -> v8::Local<'a, v8::Object> {
        self.create_error(ExceptionType::RangeError, message)
    }

    /// Like [`Lock::error`], but builds the message from raw bytes interpreted
    /// as UTF-8.
    ///
    /// Invalid UTF-8 sequences are replaced with U+FFFD rather than rejected,
    /// so this accepts arbitrary byte input (e.g. filesystem paths).
    pub fn error_from_bytes<'a>(&mut self, message: &[u8]) -> v8::Local<'a, v8::Object> {
        self.create_error_from_bytes(ExceptionType::Error, message)
    }

    /// Like [`Lock::type_error`], but builds the message from raw bytes.
    /// See [`Lock::error_from_bytes`].
    pub fn type_error_from_bytes<'a>(&mut self, message: &[u8]) -> v8::Local<'a, v8::Object> {
        self.create_error_from_bytes(ExceptionType::TypeError, message)
    }

    /// Like [`Lock::range_error`], but builds the message from raw bytes.
    /// See [`Lock::error_from_bytes`].
    pub fn range_error_from_bytes<'a>(&mut self, message: &[u8]) -> v8::Local<'a, v8::Object> {
        self.create_error_from_bytes(ExceptionType::RangeError, message)
    }

    fn create_error<'a>(
        &mut self,
        exception_type: ExceptionType,
        message: &str,
    ) -> v8::Local<'a, v8::Object> {
        // A &str is always valid UTF-8, so delegating to the byte-based path
        // produces identical output (no invalid sequences to replace) while
        // keeping a single exception-construction implementation.
        self.create_error_from_bytes(exception_type, message.as_bytes())
    }

    fn create_error_from_bytes<'a>(
        &mut self,
        exception_type: ExceptionType,
        message: &[u8],
    ) -> v8::Local<'a, v8::Object> {
        // Strings longer than i32::MAX bytes are truncated to satisfy V8's
        // int-typed length parameter; V8 rejects strings that large anyway.
        let len = i32::try_from(message.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is valid and locked; message.as_ptr()
        // and len describe a valid byte slice.
        let value: v8::Local<'a, v8::Value> = unsafe {
            v8::Local::from_ffi(
                self.isolate(),
                v8::ffi::exception_create_from_bytes(
                    self.isolate().as_ffi(),
                    exception_type,
                    message.as_ptr(),
                    len,
                ),
            )
        };
        // Error, TypeError, and RangeError are all JS objects.
        value.into()
    }

    pub fn throw_error(&mut self, message: &str) {
        // SAFETY: Lock guarantees the isolate is valid and locked
        unsafe { v8::ffi::isolate_throw_error(self.isolate().as_ffi(), message) }
    }

    pub fn await_io<F, C, I, R>(self, _fut: F, _callback: C) -> Result<R>
    where
        F: Future<Output = I>,
        C: FnOnce(Self, I) -> Result<R>,
    {
        unimplemented!("Lock::await_io is not yet implemented for Rust resources")
    }

    pub(crate) fn realm(&mut self) -> &mut Realm {
        // SAFETY: isolate is valid and locked (guaranteed by Lock); Realm pointer is valid.
        unsafe { &mut *crate::ffi::realm_from_isolate(self.isolate().as_ffi()) }
    }

    /// Returns the current worker's compatibility flags reader.
    ///
    /// ```ignore
    /// if lock.feature_flags().get_node_js_compat() {
    ///     // Node.js compatibility behavior
    /// }
    /// ```
    pub fn feature_flags(&mut self) -> compatibility_date_capnp::compatibility_flags::Reader<'_> {
        self.realm().feature_flags.reader()
    }

    /// Throws an error as a V8 exception.
    ///
    /// If `err.is_termination` is set, no JS exception is scheduled; termination
    /// is re-armed instead so V8 unwinds all JS frames (a termination "throw"
    /// must never be catchable by guest JS).
    ///
    /// If `err.is_internal` is set, the message is redacted (via
    /// `throw_internal_error()`) rather than thrown verbatim, matching how
    /// C++ `decodeTunneledException()` handles `isInternal` KJ exceptions.
    pub fn throw_exception(&mut self, err: &Error) {
        if err.is_termination {
            // Idempotent if termination is already pending; covers the edge case
            // where V8 cleared the flag after unwinding all JS frames.
            self.terminate_execution();
            return;
        }
        if err.is_internal {
            self.throw_internal_error(&err.message);
            return;
        }
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe {
            v8::ffi::isolate_throw_exception(
                self.isolate().as_ffi(),
                err.to_local(self.isolate()).into_ffi(),
            );
        }
    }

    /// Throws an internal error as a V8 exception, matching `makeInternalError()` in C++.
    ///
    /// Generates a unique error reference ID, logs `internal_message` with the ID via
    /// `KJ_LOG(ERROR)` (reaching Sentry), and schedules a generic
    /// `"Error: internal error; reference = <id>"` JS exception on the isolate so the
    /// internal message is never exposed to JavaScript.
    pub fn throw_internal_error(&mut self, internal_message: &str) {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe {
            v8::ffi::isolate_throw_internal_error(self.isolate().as_ffi(), internal_message);
        }
    }

    /// Signals that JavaScript execution on this isolate should be terminated immediately.
    ///
    /// Calls `IsolateBase::TerminateExecution()`, so V8 raises an
    /// uncatchable termination exception that unwinds all JS call frames back
    /// to the top-level C++ entry point.
    ///
    /// This mirrors what C++ `KJ_ASSERT` / `KJ_FAIL_ASSERT` effectively do when they fire
    /// inside an isolate context: they abort further JS execution rather than letting the
    /// isolate continue in a potentially inconsistent state.
    pub fn terminate_execution(&mut self) {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe {
            v8::ffi::isolate_terminate_execution(self.isolate().as_ffi());
        }
    }
}

/// Provides metadata about Rust types exposed to JavaScript.
///
/// This trait provides type information used for error messages, memory tracking,
/// and type validation (for `NonCoercible<T>`). The actual conversion logic is in
/// `ToJS` (Rust → JS) and `FromJS` (JS → Rust).
///
/// TODO: Implement `memory_info(jsg::MemoryTracker)`
pub trait Type: Sized {
    /// The JavaScript class name for this type (used in error messages).
    fn class_name() -> &'static str;

    /// Same as jsgGetMemorySelfSize
    fn memory_self_size() -> usize {
        std::mem::size_of::<Self>()
    }

    /// Returns true if the V8 value is exactly this type (no coercion).
    /// Used by `NonCoercible<T>` to reject values that would require coercion.
    fn is_exact(value: &v8::Local<v8::Value>) -> bool;
}

/// Represents a constant value that can be exposed to JavaScript.
pub enum ConstantValue {
    Number(f64),
}

macro_rules! impl_constant_value_from_lossless {
    ($($t:ty),*) => {
        $(
            impl From<$t> for ConstantValue {
                fn from(v: $t) -> Self {
                    Self::Number(f64::from(v))
                }
            }
        )*
    };
}

impl_constant_value_from_lossless!(i8, i16, i32, u8, u16, u32, f32, f64);

// i64/u64 can lose precision in f64 (52-bit mantissa), but JavaScript numbers are
// always f64 so this is inherent to the language boundary.
impl From<i64> for ConstantValue {
    #[expect(clippy::cast_precision_loss)]
    fn from(v: i64) -> Self {
        Self::Number(v as f64)
    }
}

impl From<u64> for ConstantValue {
    #[expect(clippy::cast_precision_loss)]
    fn from(v: u64) -> Self {
        Self::Number(v as f64)
    }
}

/// Where a [`Member::Property`] is attached on the JavaScript object.
///
/// This is a re-export of the CXX bridge type in [`v8::ffi`] so that callers
/// do not need to import `jsg::v8::ffi` directly.  The three variants behave
/// as follows:
///
/// - `Prototype` — accessor on the prototype chain; enumerable.
/// - `Instance`  — own accessor on every instance; enumerable.
/// - `Inspect`   — symbol-keyed on the prototype; hidden from normal enumeration,
///   surfaced by `node:util` `inspect()`.
pub use crate::v8::ffi::PropertyKind;

pub enum Member {
    Constructor {
        callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
    Method {
        name: String,
        callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
    /// A property accessor with configurable placement. `setter_callback = None`
    /// makes the property read-only; `Inspect` properties are always read-only.
    Property {
        name: String,
        kind: PropertyKind,
        getter_callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
        /// `None` for read-only properties.
        setter_callback: Option<unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo)>,
    },
    StaticMethod {
        name: String,
        callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
    StaticConstant {
        name: String,
        value: ConstantValue,
    },
}

/// Trait for types that participate in V8 garbage collection as tracked resources.
///
/// Extends [`Traced`] (which provides the `trace` method for visiting nested
/// GC-visible references) with a class name for heap snapshot tooling.
///
/// `#[jsg_resource]` auto-derives both `Traced` and `GarbageCollected`.
/// Use `#[jsg_resource(custom_trace)]` to suppress the generated `Traced`
/// impl and provide your own.
pub trait GarbageCollected: Traced {
    /// Class name for heap snapshots / debugging.
    ///
    /// Returns a `&'static CStr` — always a compile-time literal. This lets the C++ side
    /// construct a `kj::StringPtr` directly from the NUL-terminated pointer without any
    /// allocation or caching. Implementations must not access `self`.
    fn memory_name(&self) -> &'static std::ffi::CStr;
}

/// Rust types that are deep-copied into JavaScript as value types.
///
/// Unlike resource types, struct types are copied entirely into JavaScript objects with no
/// further Rust involvement after wrapping. This is analogous to `JSG_STRUCT` in C++ JSG.
pub trait Struct: Type {}

/// Per-isolate state for Rust resources exposed to JavaScript.
///
/// A Realm is created for each V8 isolate and stored in the isolate's data slot. It holds
/// cached function templates and tracks resource instances. When all Rust `Ref` handles to a
/// resource are dropped and no JS references remain, V8 GC collects the wrapper and the
/// `CppgcShim` destruction triggers cleanup of the underlying `Wrappable`.
pub struct Realm {
    isolate: v8::IsolatePtr,
    pub(crate) resources: resource::Resources,
    /// Parsed `CompatibilityFlags` capnp message, initialized at construction.
    feature_flags: FeatureFlags,
}

impl Realm {
    /// Creates a new Realm with its feature flags.
    pub fn new(isolate: v8::IsolatePtr, feature_flags: FeatureFlags) -> Self {
        Self {
            isolate,
            resources: resource::Resources::default(),
            feature_flags,
        }
    }

    pub fn isolate(&self) -> v8::IsolatePtr {
        self.isolate
    }
}

impl Drop for Realm {
    fn drop(&mut self) {
        debug_assert!(
            // SAFETY: isolate pointer is valid (guaranteed by Realm construction).
            unsafe { self.isolate.is_locked() },
            "Realm must be dropped while holding the isolate lock"
        );
    }
}

#[expect(clippy::unnecessary_box_returns)]
unsafe fn realm_create(isolate: *mut v8::ffi::Isolate, feature_flags_data: &[u8]) -> Box<Realm> {
    let feature_flags = FeatureFlags::from_bytes(feature_flags_data);
    // SAFETY: isolate pointer is valid (guaranteed by C++ caller).
    unsafe { Box::new(Realm::new(v8::IsolatePtr::from_ffi(isolate), feature_flags)) }
}

/// Executes `f`, catching any panic and converting it to a JS internal error.
///
/// `extern "C"` V8 callbacks generated by `jsg-macros` call this so that a
/// Rust panic is handled the same way as `KJ_ASSERT(false)` in C++ JSG
/// handlers: the internal panic message is logged via `KJ_LOG(ERROR)` (reaching
/// Sentry) and the JS caller receives a generic
/// `"Error: internal error; reference = <id>"` exception with a unique
/// reference ID, keeping the internal message out of JS-visible output.
///
/// After logging the error, `terminate_execution()` is called so that V8 raises
/// an uncatchable termination exception and unwinds all JS call frames. This
/// mirrors what C++ `KJ_ASSERT` effectively does inside an isolate context and
/// closes the window where a panicked resource with partially-mutated `jsg::Rc`
/// fields could be observed by a subsequent GC trace before isolate teardown.
#[doc(hidden)]
pub fn catch_panic<F: FnOnce()>(lock: &mut Lock, f: F) {
    // SAFETY: `f` is called from an `extern "C"` V8 callback generated by
    // jsg-macros. Raw pointers captured by `f` are valid for the entire
    // duration of the callback (V8 guarantees this), and V8 is
    // single-threaded, so there is no aliasing hazard on unwind.
    // `AssertUnwindSafe` is therefore correct here.
    if let Err(payload) = std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        let msg = if let Some(s) = payload.downcast_ref::<&str>() {
            *s
        } else if let Some(s) = payload.downcast_ref::<String>() {
            s.as_str()
        } else {
            "<non-string panic payload>"
        };
        lock.throw_internal_error(msg);
        lock.terminate_execution();
    }
}
