// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::future::Future;
use std::num::ParseIntError;
use std::ops::Deref;
use std::pin::Pin;
use std::sync::Arc;

pub mod fast_api;
pub mod feature_flags;
mod isolate_state;
pub mod lenient;
pub mod macros;
pub mod modules;
pub mod nullable;
pub mod resource;
mod scope;
pub mod v8;
mod wrappable;

pub use feature_flags::FeatureFlags;
use isolate_state::IsolateState;
pub use lenient::Lenient;
pub use nullable::Nullable;
pub use resource::NestedType;
pub use resource::Rc;
pub use resource::Resource;
pub use resource::Weak;
pub use scope::CallbackScope;
pub use scope::CallbackScopeParam;
pub use scope::Intercepted;
pub use scope::Isolate;
pub use scope::Lock;
pub use scope::RawFunctionCallback;
pub use scope::RawGetterCallback;
pub use scope::RawSetterCallback;
pub use scope::RawWildcardGetterCallback;
pub use scope::RawWildcardQueryCallback;
pub use scope::UnitType;
pub use scope::allow_scopes_of_another_isolate;
pub use scope::function_callback;
pub use scope::getter_callback;
pub use scope::setter_callback;
pub use scope::wildcard_getter_callback;
pub use scope::wildcard_query_callback;
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
#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
mod ffi {
    extern "Rust" {
        type Realm;
        type IsolateState;

        /// Create a fully-initialized Realm with feature flags.
        /// `feature_flags_data` is canonical (single-segment, no segment table) Cap'n Proto
        /// bytes produced by `capnp::canonicalize()` on the C++ side.
        #[expect(
            clippy::unnecessary_box_returns,
            reason = "cxx takes an opaque Rust type boxed"
        )]
        unsafe fn realm_create(isolate: *mut Isolate, feature_flags_data: &[u8]) -> Box<Realm>;

        /// Drains the Realm's cached resource-template handles for the `PREPARE_SNAPSHOT`
        /// pipeline. Each returned handle is a `v8::Global<v8::FunctionTemplate>`, recovered on
        /// the C++ side with `global_from_ffi`; ownership transfers to the caller, which must
        /// dispose the handle before `CreateBlob`. The cache is left empty (templates are
        /// recreated lazily; `START_FROM_SNAPSHOT` isolates start empty anyway).
        fn realm_take_resource_templates(realm: &mut Realm) -> Vec<Global>;

        /// Resets the V8 handles that `Global`s and `TracedGlobal`s dropped without the
        /// isolate lock left for the next lock. `IsolateBase::applyDeferredActions` calls it
        /// through the deferred action hook, which `IsolateBase::Lock` does whenever it locks
        /// the isolate.
        /// The isolate must be locked by the calling thread, with no Rust `Lock` in use.
        unsafe fn realm_reset_globals_awaiting_lock(isolate: *mut Isolate);

        /// Frees the slots of `TracedGlobal`s dropped after the Realm; see
        /// `IsolateState::isolate_disposed`. `~IsolateBase` calls it after
        /// `v8::Isolate::Dispose`, through the hook that `realm_detach` installs, with the
        /// reference to the state that `realm_detach` was given.
        unsafe fn isolate_disposed(state: *const IsolateState);
    }

    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = crate::v8::ffi::Isolate;
        type Global = crate::v8::ffi::Global;

        // Realm
        pub unsafe fn realm_from_isolate(isolate: *mut Isolate) -> *mut Realm;
        /// Whether `realm_attach` may attach a Realm to the isolate: none is attached, and
        /// none has been detached, which sets the dispose hook.
        unsafe fn realm_can_attach(isolate: *mut Isolate) -> bool;
        /// Stores `realm` in the isolate's data slot and installs the deferred action hook
        /// that calls `realm_reset_globals_awaiting_lock`.
        pub unsafe fn realm_attach(isolate: *mut Isolate, realm: &mut Realm);
        /// Undoes `realm_attach`, and sets the `IsolateBase` dispose hook to one that hands
        /// `state`, a reference to the state from `Arc::into_raw`, to `isolate_disposed` once
        /// the isolate is disposed.
        pub unsafe fn realm_detach(isolate: *mut Isolate, state: *const IsolateState);
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
    /// Whether this error stands for an exception or termination that is pending in V8; see
    /// [`Error::is_pending`].
    pending: bool,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.name, self.message)
    }
}

/// Generates constructor methods for each `ExceptionType` variant.
/// e.g., `new_type_error("message")` creates an Error with `ExceptionType::TypeError`.
macro_rules! impl_error_constructors {
    ($($variant:ident => $fn_name:ident),* $(,)?) => {
        impl Error {
            $(
                pub fn $fn_name(message: impl Into<String>) -> Self {
                    Self {
                        name: ExceptionType::$variant,
                        message: message.into(),
                        is_internal: false,
                        pending: false,
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

impl<'s> FromJS<'s> for Error {
    type ResultType = Self;

    /// Creates an Error from a V8 value (typically an exception).
    ///
    /// If the value is a native error, extracts the name and message properties.
    /// Otherwise, converts the value to a string for the message.
    ///
    /// Reading `name` and `message` can run JavaScript (a getter, a Proxy trap, or `toString`).
    /// If it throws, or execution is terminating, this fails with an error for which
    /// [`Error::is_pending`] is true.
    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        if value.is_native_error()
            && let Ok(obj) = v8::Local::<v8::Object>::try_from(value)
        {
            let name = obj.get(lock, "name")?;
            let name = if name.is_undefined() {
                None
            } else {
                Some(String::from_js(lock, name)?)
            };

            let message = obj.get(lock, "message")?;
            let message = if message.is_undefined() {
                "Unknown error".to_owned()
            } else {
                String::from_js(lock, message)?
            };

            Ok(Self {
                name: name.map_or(ExceptionType::Error, |n| ExceptionType::from(n.as_str())),
                message,
                is_internal: false,
                pending: false,
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
            pending: false,
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
            pending: false,
        }
    }

    /// Creates an `Error` for a call into JavaScript that threw, or that execution terminating
    /// interrupted, leaving the exception pending in V8; see [`Error::is_pending`].
    pub(crate) fn pending() -> Self {
        Self {
            pending: true,
            ..Self::new_internal_error("a JavaScript exception is pending")
        }
    }

    /// Returns whether this error stands for an exception or termination that is pending in V8,
    /// as after a call into JavaScript that threw.
    ///
    /// [`Lock::throw_exception`] throws nothing for such an error, so returning it from a
    /// callback propagates the pending exception to the callback's JavaScript caller unchanged,
    /// as C++ JSG's `JsExceptionThrown` does. `name` and `message` don't describe the exception;
    /// to inspect it, catch it with [`Lock::try_catch`]. Code that gets such an error must not
    /// call into JavaScript, and should return it; converting it to another error and throwing
    /// that replaces the exception (but never a termination). The error does not track its
    /// exception: kept after the exception was caught or propagated, as a clone can be, and
    /// thrown with no exception pending, it becomes an internal error.
    #[must_use]
    pub const fn is_pending(&self) -> bool {
        self.pending
    }

    /// Creates a V8 exception from this error.
    ///
    /// This always builds the exception from `self.message` verbatim: it does
    /// not check `is_internal` / redact anything. Prefer `Lock::throw_exception()`,
    /// which does perform that check before falling back to this method.
    #[expect(
        unsafe_code,
        reason = "creates the V8 exception object through the FFI"
    )]
    pub fn to_local<'s>(&self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
        let isolate = lock.isolate();
        let message = self.message.as_bytes();
        // Strings longer than i32::MAX bytes are truncated to satisfy V8's
        // int-typed length parameter; V8 rejects strings that large anyway.
        let len = i32::try_from(message.len()).unwrap_or(i32::MAX);
        // SAFETY: isolate is valid and locked (guaranteed by caller); message
        // pointer and len describe a valid byte slice.
        unsafe {
            v8::Local::from_ffi(v8::ffi::exception_create_from_bytes(
                isolate.as_ffi(),
                self.name,
                message.as_ptr(),
                len,
            ))
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

        if let Some(rest) = body.strip_prefix("DOMException(")
            && let Some((name, message)) = rest.split_once("): ")
        {
            return Some(Self {
                name: ExceptionType::from(name),
                message: message.to_owned(),
                is_internal,
                pending: false,
            });
        }

        let (ty, message) = body.split_once(": ")?;
        Some(Self {
            name: ExceptionType::from(ty),
            message: message.to_owned(),
            is_internal,
            pending: false,
        })
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
            pending: false,
        }
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
    #[expect(
        clippy::float_cmp,
        reason = "exact comparison is intended: an integer equals its own truncation"
    )]
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

#[expect(
    unsafe_code,
    reason = "`Lock` holds the raw isolate pointer and calls V8 through the FFI"
)]
impl<'s> Lock<'s> {
    /// Returns `undefined`.
    pub fn undefined(&mut self) -> v8::Local<'s, v8::Value> {
        // SAFETY: Lock guarantees the isolate is valid and locked.
        unsafe { v8::Local::from_ffi(v8::ffi::local_new_undefined(self.isolate().as_ffi())) }
    }

    /// Returns `null`.
    pub fn null(&mut self) -> v8::Local<'s, v8::Value> {
        // SAFETY: Lock guarantees the isolate is valid and locked.
        unsafe { v8::Local::from_ffi(v8::ffi::local_new_null(self.isolate().as_ffi())) }
    }

    pub fn new_object(&mut self) -> v8::Local<'s, v8::Object> {
        // SAFETY: Lock guarantees the isolate is valid and locked
        unsafe { v8::Local::from_ffi(v8::ffi::local_new_object(self.isolate().as_ffi())) }
    }

    /// Creates a JavaScript `Error` object with the given message.
    ///
    /// Mirrors C++ `jsg::Lock::error()`. The returned object can have additional
    /// properties attached before being thrown or returned.
    pub fn error(&mut self, message: &str) -> v8::Local<'s, v8::Object> {
        self.create_error(ExceptionType::Error, message)
    }

    /// Creates a JavaScript `TypeError` object with the given message.
    ///
    /// Mirrors C++ `jsg::Lock::typeError()`.
    pub fn type_error(&mut self, message: &str) -> v8::Local<'s, v8::Object> {
        self.create_error(ExceptionType::TypeError, message)
    }

    /// Creates a JavaScript `RangeError` object with the given message.
    ///
    /// Mirrors C++ `jsg::Lock::rangeError()`.
    pub fn range_error(&mut self, message: &str) -> v8::Local<'s, v8::Object> {
        self.create_error(ExceptionType::RangeError, message)
    }

    /// Like [`Lock::error`], but builds the message from raw bytes interpreted
    /// as UTF-8.
    ///
    /// Invalid UTF-8 sequences are replaced with U+FFFD rather than rejected,
    /// so this accepts arbitrary byte input (e.g. filesystem paths).
    pub fn error_from_bytes(&mut self, message: &[u8]) -> v8::Local<'s, v8::Object> {
        self.create_error_from_bytes(ExceptionType::Error, message)
    }

    /// Like [`Lock::type_error`], but builds the message from raw bytes.
    /// See [`Lock::error_from_bytes`].
    pub fn type_error_from_bytes(&mut self, message: &[u8]) -> v8::Local<'s, v8::Object> {
        self.create_error_from_bytes(ExceptionType::TypeError, message)
    }

    /// Like [`Lock::range_error`], but builds the message from raw bytes.
    /// See [`Lock::error_from_bytes`].
    pub fn range_error_from_bytes(&mut self, message: &[u8]) -> v8::Local<'s, v8::Object> {
        self.create_error_from_bytes(ExceptionType::RangeError, message)
    }

    fn create_error(
        &mut self,
        exception_type: ExceptionType,
        message: &str,
    ) -> v8::Local<'s, v8::Object> {
        // A &str is always valid UTF-8, so delegating to the byte-based path
        // produces identical output (no invalid sequences to replace) while
        // keeping a single exception-construction implementation.
        self.create_error_from_bytes(exception_type, message.as_bytes())
    }

    fn create_error_from_bytes(
        &mut self,
        exception_type: ExceptionType,
        message: &[u8],
    ) -> v8::Local<'s, v8::Object> {
        // Strings longer than i32::MAX bytes are truncated to satisfy V8's
        // int-typed length parameter; V8 rejects strings that large anyway.
        let len = i32::try_from(message.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is valid and locked; message.as_ptr()
        // and len describe a valid byte slice. V8's error constructors always return an
        // object.
        let error: v8::Local<'s, v8::Object> = unsafe {
            v8::Local::from_ffi(v8::ffi::exception_create_from_bytes(
                self.isolate().as_ffi(),
                exception_type,
                message.as_ptr(),
                len,
            ))
        };
        debug_assert!(error.is_object());
        error
    }

    pub fn throw_error(&mut self, message: &str) {
        // SAFETY: Lock guarantees the isolate is valid and locked
        unsafe { v8::ffi::isolate_throw_error(self.isolate().as_ffi(), message) }
    }

    /// Runs `fut` as I/O and later calls `callback` with its output under a new lock. Both run
    /// after the current callback has returned, so they must be `'static`.
    #[expect(
        clippy::unimplemented,
        reason = "`await_io` is not implemented for Rust resources yet"
    )]
    pub fn await_io<F, C, I, R>(&mut self, _fut: F, _callback: C) -> Result<R>
    where
        F: Future<Output = I> + 'static,
        C: for<'t> FnOnce(&mut Lock<'t>, I) -> Result<R> + 'static,
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

    /// The number of `Global`s and `TracedGlobal`s that were dropped without the isolate lock
    /// and whose handles wait for the next lock to be reset; see [`v8::Global`]. For tests.
    #[doc(hidden)]
    #[must_use]
    pub fn globals_awaiting_lock(&mut self) -> usize {
        self.realm().state.awaiting_lock_count()
    }

    /// Throws an error as a V8 exception.
    ///
    /// Throws nothing for an error that stands for a pending exception (see
    /// [`Error::is_pending`]), which then keeps propagating, nor while execution is terminating,
    /// so that the termination is never replaced by a catchable exception. Such an error can
    /// outlive its exception, as a clone kept after [`Lock::try_catch`] caught it; with no
    /// exception pending, it is thrown as an internal error. Otherwise, if
    /// `err.is_internal` is set, the message is redacted (via
    /// `throw_internal_error()`) rather than thrown verbatim, matching how
    /// C++ `decodeTunneledException()` handles `isInternal` KJ exceptions.
    pub fn throw_exception(&mut self, err: &Error) {
        if err.is_pending() {
            let isolate = self.isolate().as_ffi();
            // SAFETY: isolate is valid and locked (guaranteed by Lock).
            if unsafe {
                v8::ffi::isolate_is_execution_terminating(isolate)
                    || v8::ffi::isolate_has_pending_exception(isolate)
            } {
                return;
            }
            // Throwing nothing would return to JavaScript with neither a result nor an
            // exception.
            self.throw_internal_error(
                "a jsg::Error for a pending JavaScript exception was thrown with no exception \
                 pending",
            );
            return;
        }
        if err.is_internal {
            self.throw_internal_error(&err.message);
            return;
        }
        if self.is_execution_terminating() {
            // `isolate_throw_exception` would throw nothing, and creating the exception while
            // execution is terminating fails a V8 debug check.
            return;
        }
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe {
            v8::ffi::isolate_throw_exception(
                self.isolate().as_ffi(),
                err.to_local(self).into_ffi(),
            );
        }
    }

    /// Runs `f`, catching what JavaScript throws while it runs, as C++ `jsg::Lock::tryCatch()`
    /// does.
    ///
    /// Returns `Ok(Ok(_))` with what `f` returns, or `Ok(Err(exception))` with the value thrown
    /// if `f` fails: what JavaScript threw for an error that stands for a pending exception (see
    /// [`Error::is_pending`]), or the error `f` returned, thrown as [`Lock::throw_exception`]
    /// throws it. A termination is not caught: it keeps propagating, and this returns `Err` with
    /// an error for which [`Error::is_pending`] is true, which the caller should return.
    ///
    /// # Errors
    ///
    /// Fails if execution is terminating.
    ///
    /// # Example
    ///
    /// ```ignore
    /// match lock.try_catch(|lock| callback.call::<Number>(lock, None, &[]))? {
    ///     Ok(result) => { /* ... */ }
    ///     Err(exception) => { /* handle what the callback threw */ }
    /// }
    /// ```
    #[expect(unsafe_code, reason = "runs `f` in a C++ v8::TryCatch through the FFI")]
    pub fn try_catch<T, F>(&mut self, f: F) -> Result<Result<T, v8::Local<'s, v8::Value>>>
    where
        F: FnOnce(&mut Self) -> Result<T>,
    {
        struct Call<'a, 's, F, T> {
            lock: &'a mut Lock<'s>,
            f: Option<F>,
            result: Option<std::thread::Result<Result<T>>>,
        }

        /// Runs the closure, and returns whether it failed: then the `TryCatch` holds what it
        /// threw. Only `try_catch` passes it, with the address of its `Call`, to C++, so it
        /// is not an `unsafe fn` (which the bridge does not accept for callbacks).
        fn trampoline<'s, F, T>(data: usize) -> bool
        where
            F: FnOnce(&mut Lock<'s>) -> Result<T>,
        {
            // SAFETY: `data` was cast from `&raw mut Call` in `try_catch`, which outlives the
            // call.
            let call = unsafe { &mut *(data as *mut Call<'_, 's, F, T>) };
            let Some(f) = call.f.take() else {
                unreachable!()
            };
            // A panic must not unwind into C++; it is resumed once the `TryCatch` is gone.
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let result = f(call.lock);
                if let Err(err) = &result {
                    // Thrown in the `TryCatch`, so that it catches what `f` failed with.
                    call.lock.throw_exception(err);
                }
                result
            }));
            let failed = !matches!(result, Ok(Ok(_)));
            call.result = Some(result);
            failed
        }

        let isolate = self.isolate().as_ffi();
        let mut call = Call {
            lock: self,
            f: Some(f),
            result: None,
        };
        let mut exception = v8::ffi::MaybeLocal { ptr: 0 };
        // SAFETY: the lock keeps the isolate locked, with a scope open, for the call; `call`
        // outlives it; `trampoline` matches its type.
        unsafe {
            v8::ffi::isolate_try_catch(
                isolate,
                &raw mut call as usize,
                trampoline::<F, T>,
                &mut exception,
            );
        }
        let Some(result) = call.result else {
            unreachable!()
        };
        match result {
            Ok(Ok(value)) => Ok(Ok(value)),
            Err(payload) => std::panic::resume_unwind(payload),
            // SAFETY: the exception belongs to the scope of the lock, which `isolate_try_catch`
            // opened no scope inside.
            Ok(Err(_)) => match unsafe { v8::Local::from_ffi_maybe(exception) } {
                Some(exception) => Ok(Err(exception)),
                None => Err(Error::pending()),
            },
        }
    }

    /// Throws an internal error as a V8 exception, matching `makeInternalError()` in C++.
    ///
    /// Generates a unique error reference ID, logs `internal_message` with the ID via
    /// `KJ_LOG(ERROR)` (reaching Sentry), and schedules a generic
    /// `"Error: internal error; reference = <id>"` JS exception on the isolate so the
    /// internal message is never exposed to JavaScript. While execution is terminating, it
    /// still logs the message but throws nothing.
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
    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool;
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
    #[expect(
        clippy::cast_precision_loss,
        reason = "a JavaScript number is an f64, so the loss is inherent to the boundary"
    )]
    fn from(v: i64) -> Self {
        Self::Number(v as f64)
    }
}

impl From<u64> for ConstantValue {
    #[expect(
        clippy::cast_precision_loss,
        reason = "a JavaScript number is an f64, so the loss is inherent to the boundary"
    )]
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
/// - `Instance`  — own data property on every instance; enumerable. Only
///   [`Member::InstanceProperty`] registers these.
/// - `Inspect`   — symbol-keyed on the prototype; hidden from normal enumeration,
///   surfaced by `node:util` `inspect()`.
/// - `LazyInstance` / `LazyReadonlyInstance` — own data property on every
///   instance, computed once on first access. Only
///   [`Member::LazyInstanceProperty`] registers these.
pub use crate::v8::ffi::PropertyKind;

/// Placement of an ordinary resource property registered by a generated member factory.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PropertyPlacement {
    /// An accessor on the prototype; preferred for new APIs.
    Prototype,
    /// An own native data property on each instance, for compatibility with existing APIs.
    Instance,
    /// An own data property on each instance whose getter runs on first access, after which
    /// the result is stored as the property's value.
    LazyInstance,
}

pub enum Member {
    Constructor {
        callback: RawFunctionCallback,
    },
    Method {
        name: String,
        callback: RawFunctionCallback,
        fast: Option<v8::ffi::FastApiDescriptor>,
    },
    /// A property accessor on the prototype (`Prototype` or `Inspect`; `Instance`
    /// panics). `setter_callback = None` makes the property read-only; `Inspect`
    /// properties are always read-only.
    Property {
        name: String,
        kind: PropertyKind,
        getter_callback: RawFunctionCallback,
        /// `None` for read-only properties.
        setter_callback: Option<RawFunctionCallback>,
    },
    /// An own data property on every instance, computed by native callbacks
    /// (V8's `SetNativeDataProperty`). `setter_callback = None` makes it
    /// read-only.
    InstanceProperty {
        name: String,
        getter_callback: RawGetterCallback,
        /// `None` for read-only properties.
        setter_callback: Option<RawSetterCallback>,
    },
    /// An own data property on every instance whose getter runs on first access
    /// (V8's `SetLazyDataProperty`). V8 then replaces it with a data property
    /// holding the getter's result, so later reads do not call the getter. The
    /// property is writable unless `read_only`, like C++
    /// `JSG_LAZY_INSTANCE_PROPERTY` / `JSG_LAZY_READONLY_INSTANCE_PROPERTY`.
    LazyInstanceProperty {
        name: String,
        getter_callback: RawGetterCallback,
        read_only: bool,
    },
    StaticMethod {
        name: String,
        callback: RawFunctionCallback,
        fast: Option<v8::ffi::FastApiDescriptor>,
    },
    StaticConstant {
        name: String,
        value: ConstantValue,
    },
    /// A property of every instance with any string name that is not a defined property of the
    /// instance or its prototypes, like C++ JSG's `JSG_WILDCARD_PROPERTY`. A type has at most
    /// one. `query_callback` tells V8 whether a property exists, for `in`.
    WildcardProperty {
        getter_callback: RawWildcardGetterCallback,
        query_callback: RawWildcardQueryCallback,
    },
    /// Another resource type's constructor, as a property of this type's
    /// prototype named `name`, like C++ JSG's `JSG_NESTED_TYPE`. Nested types
    /// are installed after every other member, whatever their position.
    NestedType {
        name: String,
        nested_type: NestedType,
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
///
/// A Realm attaches itself to its isolate: it is stored in the isolate's data slot, and it
/// installs the hook through which each `IsolateBase::Lock` resets the persistent handles
/// dropped without the lock. It must be dropped under the isolate lock, before the isolate is
/// disposed: dropping it detaches it and closes the isolate's [`IsolateState`], after which the
/// `Global`s and `TracedGlobal`s of the isolate leave their handles to `v8::Isolate::Dispose`.
/// The isolate keeps the state alive until it is disposed.
pub struct Realm {
    isolate: v8::IsolatePtr,
    /// Shared with every `Global` and `TracedGlobal` of the isolate, which identifies its isolate
    /// by it.
    state: Arc<IsolateState>,
    resources: resource::Resources,
    /// Parsed `CompatibilityFlags` capnp message, initialized at construction.
    feature_flags: FeatureFlags,
}

impl Realm {
    /// Creates a new Realm with its feature flags.
    pub fn new(isolate: v8::IsolatePtr, feature_flags: FeatureFlags) -> Self {
        Self {
            isolate,
            state: IsolateState::new(isolate),
            resources: resource::Resources::default(),
            feature_flags,
        }
    }

    pub fn isolate(&self) -> v8::IsolatePtr {
        self.isolate
    }

    pub(crate) fn state(&self) -> &Arc<IsolateState> {
        &self.state
    }

    /// Gets or creates the cached `FunctionTemplate` for a resource type, built against
    /// this realm's compatibility flags.
    pub(crate) fn get_constructor<R: Resource>(&mut self) -> &v8::Global<v8::FunctionTemplate> {
        self.resources
            .get_constructor::<R>(self.isolate, &self.state, self.feature_flags.reader())
    }
}

#[expect(
    unsafe_code,
    reason = "detaches from the isolate, which must be locked, and closes its state"
)]
impl Drop for Realm {
    fn drop(&mut self) {
        debug_assert!(
            // SAFETY: isolate pointer is valid (guaranteed by Realm construction).
            unsafe { self.isolate.is_locked() },
            "Realm must be dropped while holding the isolate lock"
        );
        // Dropped before the state is closed, so that their handles are reset rather than
        // left to `Dispose`.
        drop(std::mem::take(&mut self.resources));
        // SAFETY: the Realm is dropped under the isolate lock, while no `Lock` is in use
        // (guaranteed by the embedder; see the type's documentation).
        let isolate = unsafe { Isolate::from_raw(self.isolate.as_ffi()) };
        self.state.close(isolate);
        // The isolate gives this reference back once it is disposed.
        let state = Arc::into_raw(Arc::clone(&self.state));
        // SAFETY: the isolate is valid and locked, and this Realm is the one attached to it.
        unsafe { ffi::realm_detach(isolate.as_ffi(), state) };
    }
}

#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx takes an opaque Rust type boxed"
)]
#[expect(unsafe_code, reason = "wraps the isolate pointer C++ passes in")]
unsafe fn realm_create(isolate: *mut v8::ffi::Isolate, feature_flags_data: &[u8]) -> Box<Realm> {
    let feature_flags = FeatureFlags::from_bytes(feature_flags_data);
    // SAFETY: isolate pointer is valid and locked (guaranteed by C++ caller).
    let mut realm =
        unsafe { Box::new(Realm::new(v8::IsolatePtr::from_ffi(isolate), feature_flags)) };
    debug_assert!(
        // SAFETY: as above.
        unsafe { ffi::realm_can_attach(isolate) },
        "an isolate has at most one Realm"
    );
    // SAFETY: as above; the Realm keeps its address for as long as it is attached, as it
    // lives in the `Box` until `Realm::drop` detaches it.
    unsafe { ffi::realm_attach(isolate, &mut realm) };
    realm
}

/// See the bridge declaration.
#[expect(
    unsafe_code,
    reason = "takes back the reference to the state that C++ held"
)]
unsafe fn isolate_disposed(state: *const IsolateState) {
    // SAFETY: `state` comes from `Arc::into_raw` in `Realm::drop`, and C++ hands it back once.
    let state = unsafe { Arc::from_raw(state) };
    state.isolate_disposed();
}

/// See the bridge declaration.
#[expect(unsafe_code, reason = "called by C++ with a raw isolate pointer")]
unsafe fn realm_reset_globals_awaiting_lock(isolate: *mut v8::ffi::Isolate) {
    // A Rust scope of the isolate open on this thread means that C++ locked the isolate again
    // under a `Lock`, which may hold a reference to the Realm. The queue then waits for the
    // next lock.
    let nested = scope::scope_owner::current() == isolate;
    debug_assert!(
        !nested,
        "the isolate was locked again while a Rust scope of it is open"
    );
    if nested {
        return;
    }
    // SAFETY: the isolate is valid and locked by this thread, and no `Lock` is in use
    // (guaranteed by the C++ caller, and checked above).
    let isolate = unsafe { Isolate::from_raw(isolate) };
    // SAFETY: the Realm is attached while the hook that calls this is installed.
    let realm = unsafe { &*ffi::realm_from_isolate(isolate.as_ffi()) };
    realm.state.reset_awaiting_lock(isolate);
}

/// See the bridge declaration: drains the cached resource-template persistent handles for
/// the `PREPARE_SNAPSHOT` pipeline.
fn realm_take_resource_templates(realm: &mut Realm) -> Vec<v8::ffi::Global> {
    realm.resources.take_template_handles()
}

/// Opens `scope` and runs `f` with its lock, catching any panic and converting it to a JS
/// internal error. Returns what `f` returns, or `None` if it panicked.
///
/// Rust code that C++ or V8 calls with a scope open runs through this, so that a Rust panic
/// is handled the same way as `KJ_ASSERT(false)` in C++ JSG handlers: the internal panic
/// message is logged via `KJ_LOG(ERROR)` (reaching Sentry) and the JS caller receives a
/// generic `"Error: internal error; reference = <id>"` exception with a unique reference ID,
/// keeping the internal message out of JS-visible output. On `None`, that exception is
/// pending, and the C++ caller should propagate it as `jsg::JsExceptionThrown`.
///
/// After logging the error, `terminate_execution()` is called so that V8 raises
/// an uncatchable termination exception and unwinds all JS call frames. This
/// mirrors what C++ `KJ_ASSERT` effectively does inside an isolate context and
/// closes the window where a panicked resource with partially-mutated `jsg::Rc`
/// fields could be observed by a subsequent GC trace before isolate teardown.
///
/// Opening the scope is part of what is caught, so Rust code entered while a scope of
/// another isolate is open on the thread throws too, instead of unwinding into C++ or V8.
/// `f`'s result cannot hold handles of the scope; convert them to their FFI form inside `f`.
#[expect(
    unsafe_code,
    reason = "throws without a lock if the scope did not open"
)]
pub fn catch_panic<F, R>(scope: Pin<&mut CallbackScope>, f: F) -> Option<R>
where
    F: for<'s> FnOnce(&'s mut Lock<'s>) -> R,
{
    let isolate = scope.isolate();
    // V8 is single-threaded and the panic terminates execution, so no JavaScript observes
    // state that `f` left half-updated.
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(scope.open()))) {
        Ok(result) => Some(result),
        Err(payload) => {
            let msg = panic_message(&*payload);
            // The scope may not have opened, so there may be no lock to throw with.
            // SAFETY: the caller of `CallbackScope::new` keeps the isolate locked, with a
            // context entered and a `HandleScope` open, while `scope` lives. The unwind closed
            // the scopes opened inside `f`.
            unsafe {
                v8::ffi::isolate_throw_internal_error(isolate.as_ffi(), msg);
                v8::ffi::isolate_terminate_execution(isolate.as_ffi());
            }
            None
        }
    }
}

fn panic_message(payload: &(dyn std::any::Any + Send)) -> &str {
    if let Some(s) = payload.downcast_ref::<&str>() {
        s
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.as_str()
    } else {
        "<non-string panic payload>"
    }
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
