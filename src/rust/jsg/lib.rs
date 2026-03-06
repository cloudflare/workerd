use std::any::Any;
use std::any::TypeId;
use std::collections::HashMap;
use std::future::Future;
use std::num::ParseIntError;
use std::ops::Deref;

use kj_rs::KjMaybe;

pub mod modules;
pub mod resource;
pub mod v8;
mod wrappable;

pub use resource::Ref;
pub use resource::ResourceImpl;
pub use resource::WeakRef;
pub use v8::BigInt64Array;
pub use v8::BigUint64Array;
pub use v8::Float32Array;
pub use v8::Float64Array;
pub use v8::GarbageCollected;
pub use v8::GcParent;
pub use v8::GcVisitor;
pub use v8::Int8Array;
pub use v8::Int16Array;
pub use v8::Int32Array;
pub use v8::IsolatePtr;
pub use v8::TracedReference;
pub use v8::Uint8Array;
pub use v8::Uint16Array;
pub use v8::Uint32Array;
pub use v8::cppgc;
pub use v8::ffi::ExceptionType;
pub use wrappable::FromJS;
pub use wrappable::ToJS;

#[cxx::bridge(namespace = "workerd::rust::jsg")]
mod ffi {
    extern "Rust" {
        type Realm;

        #[expect(clippy::unnecessary_box_returns)]
        unsafe fn realm_create(isolate: *mut Isolate) -> Box<Realm>;
    }

    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type Isolate = crate::v8::ffi::Isolate;

        // Realm
        pub unsafe fn realm_from_isolate(isolate: *mut Isolate) -> *mut Realm;
    }
}

pub type Result<T, E = Error> = std::result::Result<T, E>;

fn get_resource_descriptor<R: Resource>() -> v8::ffi::ResourceDescriptor {
    let mut descriptor = v8::ffi::ResourceDescriptor {
        name: R::class_name().to_owned(),
        constructor: KjMaybe::None,
        methods: Vec::new(),
        static_methods: Vec::new(),
    };

    for m in R::members() {
        match m {
            Member::Constructor { callback } => {
                descriptor.constructor = KjMaybe::Some(v8::ffi::ConstructorDescriptor {
                    callback: callback as usize,
                });
            }
            Member::Method { name, callback } => {
                descriptor.methods.push(v8::ffi::MethodDescriptor {
                    name,
                    callback: callback as usize,
                });
            }
            Member::Property {
                name: _,
                getter_callback: _,
                setter_callback: _,
            } => todo!(),
            Member::StaticMethod { name, callback } => {
                descriptor
                    .static_methods
                    .push(v8::ffi::StaticMethodDescriptor {
                        name,
                        callback: callback as usize,
                    });
            }
        }
    }

    descriptor
}

pub fn create_resource_constructor<R: Resource>(
    lock: &mut Lock,
) -> v8::Global<v8::FunctionTemplate> {
    // SAFETY: Lock guarantees the isolate is valid and locked
    unsafe {
        v8::ffi::create_resource_template(lock.isolate().as_ffi(), &get_resource_descriptor::<R>())
            .into()
    }
}

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
        }
    }

    /// Creates a V8 exception from this error.
    pub fn to_local<'a>(&self, isolate: v8::IsolatePtr) -> v8::Local<'a, v8::Value> {
        unsafe {
            v8::Local::from_ffi(
                isolate,
                v8::ffi::exception_create(isolate.as_ffi(), self.name, &self.message),
            )
        }
    }
}

impl From<ParseIntError> for Error {
    fn from(err: ParseIntError) -> Self {
        Self::new_range_error(format!("Failed to parse integer: {err}"))
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

/// A wrapper type that accepts `null`, `undefined`, or a value of type `T`.
///
/// `Nullable<T>` is similar to `Option<T>` but also accepts `undefined` as a null-ish value.
/// This is useful for JavaScript APIs where both `null` and `undefined` represent
/// the absence of a value.
///
/// # Behavior
///
/// - `null` → `Nullable::Null`
/// - `undefined` → `Nullable::Undefined`
/// - `T` → `Nullable::Some(T)`
///
/// # Example
///
/// ```ignore
/// use jsg::Nullable;
///
/// #[jsg_method]
/// pub fn process(&self, value: Nullable<String>) -> Result<(), Error> {
///     match value {
///         Nullable::Some(s) => println!("Got value: {}", s),
///         Nullable::Null => println!("Got null"),
///         Nullable::Undefined => println!("Got undefined"),
///     }
///     Ok(())
/// }
/// ```
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Nullable<T> {
    Some(T),
    Null,
    Undefined,
}

impl<T> Nullable<T> {
    /// Returns `true` if the nullable contains a value.
    pub fn is_some(&self) -> bool {
        matches!(self, Self::Some(_))
    }

    /// Returns `true` if the nullable is `Null`.
    pub fn is_null(&self) -> bool {
        matches!(self, Self::Null)
    }

    /// Returns `true` if the nullable is `Undefined`.
    pub fn is_undefined(&self) -> bool {
        matches!(self, Self::Undefined)
    }

    /// Returns `true` if the nullable is `Null` or `Undefined`.
    pub fn is_null_or_undefined(&self) -> bool {
        matches!(self, Self::Null | Self::Undefined)
    }

    /// Converts from `Nullable<T>` to `Option<T>`.
    pub fn into_option(self) -> Option<T> {
        match self {
            Self::Some(v) => Some(v),
            Self::Null | Self::Undefined => None,
        }
    }

    /// Returns a reference to the contained value, or `None` if null or undefined.
    pub fn as_ref(&self) -> Option<&T> {
        match self {
            Self::Some(v) => Some(v),
            Self::Null | Self::Undefined => None,
        }
    }
}

impl<T> From<Option<T>> for Nullable<T> {
    fn from(opt: Option<T>) -> Self {
        match opt {
            Some(v) => Self::Some(v),
            None => Self::Null,
        }
    }
}

impl<T> From<Nullable<T>> for Option<T> {
    fn from(nullable: Nullable<T>) -> Self {
        nullable.into_option()
    }
}

/// Provides access to V8 operations within an isolate lock.
///
/// A Lock wraps a V8 isolate pointer and is passed to resource methods and callbacks to
/// perform V8 operations like creating objects, wrapping values, and accessing the Realm.
/// This is analogous to `jsg::Lock` in C++ JSG.
pub struct Lock {
    isolate: v8::IsolatePtr,
}

impl Lock {
    /// # Safety
    /// The caller must ensure that `args` is a valid pointer to `FunctionCallbackInfo`.
    pub unsafe fn from_args(args: *mut v8::ffi::FunctionCallbackInfo) -> Self {
        unsafe { Self::from_isolate_ptr(v8::ffi::fci_get_isolate(args)) }
    }

    /// Creates a Lock from a raw isolate pointer.
    ///
    /// # Safety
    /// The caller must ensure that `isolate` is a valid pointer to an `Isolate`.
    pub unsafe fn from_isolate_ptr(isolate: *mut v8::ffi::Isolate) -> Self {
        Self {
            isolate: unsafe { v8::IsolatePtr::from_ffi(isolate) },
        }
    }

    /// Returns the isolate associated with this lock.
    pub fn isolate(&self) -> v8::IsolatePtr {
        self.isolate
    }

    pub fn is_locked(&self) -> bool {
        // SAFETY: Lock guarantees the isolate is valid
        unsafe { self.isolate.is_locked() }
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

    pub fn throw_error(&mut self, message: &str) {
        // SAFETY: Lock guarantees the isolate is valid and locked
        unsafe { v8::ffi::isolate_throw_error(self.isolate().as_ffi(), message) }
    }

    pub fn await_io<F, C, I, R>(self, _fut: F, _callback: C) -> Result<R>
    where
        F: Future<Output = I>,
        C: FnOnce(Self, I) -> Result<R>,
    {
        todo!()
    }

    pub fn realm(&mut self) -> &mut Realm {
        unsafe { &mut *crate::ffi::realm_from_isolate(self.isolate().as_ffi()) }
    }

    /// Throws an error as a V8 exception.
    pub fn throw_exception(&mut self, err: &Error) {
        unsafe {
            v8::ffi::isolate_throw_exception(
                self.isolate().as_ffi(),
                err.to_local(self.isolate()).into_ffi(),
            );
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

    /// Same as jsgGetMemoryName
    fn memory_name() -> &'static str {
        std::any::type_name::<Self>()
    }

    /// Same as jsgGetMemorySelfSize
    fn memory_self_size() -> usize {
        std::mem::size_of::<Self>()
    }

    /// Returns true if the V8 value is exactly this type (no coercion).
    /// Used by `NonCoercible<T>` to reject values that would require coercion.
    fn is_exact(value: &v8::Local<v8::Value>) -> bool;
}

pub enum Member {
    Constructor {
        callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
    Method {
        name: String,
        callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
    Property {
        name: String,
        getter_callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
        setter_callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
    StaticMethod {
        name: String,
        callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
}

/// Rust types exposed to JavaScript as resource types.
///
/// Resource types are passed by reference and call back into Rust when JavaScript accesses
/// their members. This is analogous to `JSG_RESOURCE_TYPE` in C++ JSG. Resources must provide
/// member declarations, a cleanup function for GC, and access to their V8 wrapper state.
pub trait Resource: Type + GarbageCollected + Sized + 'static {
    type Template: ResourceTemplate;

    /// Returns the list of methods, properties, and constructors exposed to JavaScript.
    fn members() -> Vec<Member>
    where
        Self: Sized;

    /// Allocates a resource instance on the cppgc heap and returns a persistent handle.
    ///
    /// The returned `Ref<Self>` manages the resource's lifetime. When all `Ref` handles are
    /// dropped, the instance becomes eligible for garbage collection.
    fn alloc(lock: &mut Lock, this: Self) -> Ref<Self> {
        resource::Instance::alloc(lock, this)
    }

    /// Wraps a resource reference for exposure to JavaScript.
    fn wrap<'a>(resource: Ref<Self>, lock: &mut Lock) -> v8::Local<'a, v8::Value>
    where
        Self::Template: 'static,
    {
        resource::wrap(lock, resource)
    }

    /// Unwraps a JavaScript value to get a reference to the underlying resource.
    ///
    /// # Safety
    /// The `value` must be a valid JavaScript wrapper for this resource type.
    fn unwrap<'a>(lock: &'a mut Lock, value: v8::Local<v8::Value>) -> &'a mut Self {
        unsafe { resource::unwrap(lock, value) }
    }
}

/// Caches the V8 `FunctionTemplate` for a resource type.
///
/// A `ResourceTemplate` is created per Realm and caches the V8 function template used to
/// instantiate JavaScript wrappers for a resource type. This avoids recreating the template
/// on every wrap operation.
pub trait ResourceTemplate {
    /// Creates a new template for the given lock, initializing the V8 function template.
    fn new(lock: &mut Lock) -> Self
    where
        Self: Sized;

    /// Returns the cached V8 function template used to create wrappers.
    fn get_constructor(&self) -> &v8::Global<v8::FunctionTemplate>;
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
/// resource are dropped, the V8 wrapper becomes weak and V8 GC will trigger cleanup via the
/// weak callback.
pub struct Realm {
    isolate: v8::IsolatePtr,
    /// Resource templates keyed by `TypeId`.
    templates: HashMap<TypeId, Box<dyn Any>>,
}

impl Realm {
    /// Creates a new Realm from a V8 isolate.
    pub fn from_isolate(isolate: v8::IsolatePtr) -> Self {
        Self {
            isolate,
            templates: HashMap::default(),
        }
    }

    pub fn isolate(&self) -> v8::IsolatePtr {
        self.isolate
    }

    /// Gets or creates the resource tracking structure for a given resource type.
    ///
    /// # Panics
    /// Panics if type mismatch (indicates a bug).
    pub fn get_resources<R: Resource + 'static>(&mut self) -> &mut ResourceImpl<R>
    where
        <R as Resource>::Template: 'static,
    {
        self.templates
            .entry(TypeId::of::<R>())
            .or_insert_with(|| Box::new(ResourceImpl::<R>::default()))
            .downcast_mut::<ResourceImpl<R>>()
            .expect("Template type mismatch")
    }
}

impl Drop for Realm {
    fn drop(&mut self) {
        debug_assert!(
            unsafe { self.isolate.is_locked() },
            "Realm must be dropped while holding the isolate lock"
        );
    }
}

#[expect(clippy::unnecessary_box_returns)]
unsafe fn realm_create(isolate: *mut v8::ffi::Isolate) -> Box<Realm> {
    unsafe { Box::new(Realm::from_isolate(v8::IsolatePtr::from_ffi(isolate))) }
}
