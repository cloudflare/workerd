use std::cell::UnsafeCell;
use std::future::Future;
use std::num::ParseIntError;
use std::ops::Deref;
use std::ops::DerefMut;
use std::os::raw::c_void;
use std::ptr::NonNull;
use std::rc::Rc;

use kj_rs::KjMaybe;

pub mod modules;
pub mod v8;
mod wrappable;

pub use v8::Int8Array;
pub use v8::Int16Array;
pub use v8::Int32Array;
pub use v8::Uint8Array;
pub use v8::Uint16Array;
pub use v8::Uint32Array;
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
    unsafe {
        v8::ffi::create_resource_template(lock.isolate().as_ffi(), &get_resource_descriptor::<R>())
            .into()
    }
}

/// Wraps a Rust resource for exposure to JavaScript.
///
/// # Safety
/// The caller must ensure V8 operations are performed within the correct isolate/context and
/// that the resource's lifetime is properly managed via the Realm.
pub unsafe fn wrap_resource<'a, R: Resource + 'a, RT: ResourceTemplate>(
    lock: &mut Lock,
    mut resource: Ref<R>,
    resource_template: &mut RT,
) -> v8::Local<'a, v8::Value> {
    match resource
        .get_state()
        .strong_wrapper
        .as_ref()
        .map(|val| val.as_local(lock))
    {
        Some(value) if value.has_value() => value.into(),
        _ => {
            let constructor = resource_template.get_constructor();

            // Store the leaked Ref in ResourceState.this and the drop function
            let drop_fn = (*resource).get_drop_fn();
            resource.get_state().this = Ref::into_raw(resource.clone()).cast();
            resource.get_state().drop_fn = Some(drop_fn);

            let instance: v8::Local<'a, v8::Value> = unsafe {
                v8::Local::from_ffi(
                    lock.isolate(),
                    v8::ffi::wrap_resource(
                        lock.isolate().as_ffi(),
                        resource.get_state().this as usize,
                        constructor.as_ffi_ref(),
                        drop_fn as usize,
                    ),
                )
            };
            unsafe {
                resource
                    .get_state()
                    .attach_wrapper(lock.realm(), instance.clone().into());
            }
            instance
        }
    }
}

pub fn unwrap_resource<'a, R: Resource>(
    lock: &'a mut Lock,
    value: v8::Local<v8::Value>,
) -> &'a mut R {
    let ptr =
        unsafe { v8::ffi::unwrap_resource(lock.isolate().as_ffi(), value.into_ffi()) as *mut R };
    unsafe { &mut *ptr }
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
/// - `NonCoercible<f64>` - only accepts JavaScript numbers
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

    pub fn new_object<'a>(&mut self) -> v8::Local<'a, v8::Object> {
        unsafe {
            v8::Local::from_ffi(
                self.isolate(),
                v8::ffi::local_new_object(self.isolate().as_ffi()),
            )
        }
    }

    pub fn await_io<F, C, I, R>(self, _fut: F, _callback: C) -> Result<R>
    where
        F: Future<Output = I>,
        C: FnOnce(Self, I) -> Result<R>,
    {
        todo!()
    }

    fn realm(&mut self) -> &mut Realm {
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

/// This is analogous to `jsg::Ref<T>` in C++ JSG.
///
/// # Thread Safety
///
/// **`Ref<T>` is not thread-safe and must not be sent or shared across threads.**
/// Resources managed by `Ref<T>` are bound to the thread of the V8 isolate in which they were created.
/// Attempting to send or access a `Ref<T>` from another thread is undefined behavior.
/// This is enforced by the use of `Rc` and `UnsafeCell`, which are not `Send` or `Sync`.
pub struct Ref<T: Resource> {
    val: Rc<UnsafeCell<T>>,
}

impl<T: Resource> Deref for Ref<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        let ptr = self.val.get();
        unsafe { &*ptr }
    }
}

impl<T: Resource> DerefMut for Ref<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        let ptr = self.val.get();
        unsafe { &mut *ptr }
    }
}

impl<T: Resource> Ref<T> {
    pub fn new(t: T) -> Self {
        Self {
            val: Rc::new(UnsafeCell::new(t)),
        }
    }

    pub fn into_raw(r: Self) -> *mut T {
        UnsafeCell::raw_get(Rc::into_raw(r.val))
    }

    /// Reconstructs a `Ref<T>` from a raw pointer.
    ///
    /// # Safety
    /// The caller must ensure:
    /// - `this` is a valid pointer that was previously created by `Ref::into_raw()`
    /// - This pointer has not been used to reconstruct a `Ref` that is still alive
    /// - The pointer is properly aligned and points to a valid `T` instance
    pub unsafe fn from_raw(this: *mut T) -> Self {
        Self {
            val: unsafe { Rc::from_raw(this.cast::<UnsafeCell<T>>()) },
        }
    }
}

impl<T: Resource> Clone for Ref<T> {
    fn clone(&self) -> Self {
        Self {
            val: self.val.clone(),
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

/// Tracks the V8 wrapper object for a Rust resource.
///
/// Each Resource embeds a `ResourceState` to maintain the connection between the Rust object and
/// its JavaScript wrapper. When a resource is wrapped, the `ResourceState` stores a weak V8 handle
/// to the wrapper object along with pointers needed for cleanup: the leaked `Ref<R>` pointer
/// and the drop function to reconstruct it. The Realm uses this state to perform deterministic
/// cleanup when the context is disposed.
pub struct ResourceState {
    pub this: *mut c_void,
    pub drop_fn: Option<unsafe extern "C" fn(*mut v8::ffi::Isolate, *mut c_void)>,
    pub strong_wrapper: Option<v8::Global<v8::Object>>,
    pub isolate: Option<v8::IsolatePtr>,
}

impl Default for ResourceState {
    fn default() -> Self {
        Self {
            this: std::ptr::null_mut(),
            drop_fn: None,
            strong_wrapper: None,
            isolate: None,
        }
    }
}

impl ResourceState {
    /// Attaches a V8 wrapper object to this resource state.
    ///
    /// # Safety
    /// The caller must ensure:
    /// - `object` is a valid V8 local object handle
    /// - This method is called from the correct V8 isolate/context
    /// - `self.this` pointer remains valid until either the weak callback fires or `Realm::drop()` is called
    ///
    /// # Panics
    /// Panics if a wrapper has already been attached (i.e., `strong_wrapper` is not `None`).
    pub unsafe fn attach_wrapper(&mut self, realm: &mut Realm, object: v8::Local<v8::Object>) {
        assert!(self.strong_wrapper.is_none());

        self.strong_wrapper = Some(object.into());
        self.isolate = Some(realm.isolate());

        realm.add_resource(NonNull::from(&mut *self));

        let Some(wrapper) = self.strong_wrapper.as_mut() else {
            unreachable!("This should not happen")
        };
        let isolate = self.isolate.expect("isolate should be set");
        unsafe {
            wrapper.make_weak(isolate, self.this, Self::weak_callback);
        }
    }

    fn weak_callback(_isolate: *mut v8::ffi::Isolate, _data: usize) {
        // TODO: Implement GC-based cleanup for resources that outlive their context.
        // Current resources like DnsUtil don't require GC cleanup.
        // All cleanup happens deterministically in Realm::drop() during context disposal.
    }
}

/// Rust types exposed to JavaScript as resource types.
///
/// Resource types are passed by reference and call back into Rust when JavaScript accesses
/// their members. This is analogous to `JSG_RESOURCE_TYPE` in C++ JSG. Resources must provide
/// member declarations, a cleanup function for GC, and access to their V8 wrapper state.
pub trait Resource: Type {
    /// Returns the list of methods, properties, and constructors exposed to JavaScript.
    fn members() -> Vec<Member>
    where
        Self: Sized;

    /// Returns the cleanup function called when V8 GC collects the wrapper or the context is
    /// disposed. This function reconstructs the leaked `Ref<R>` and drops it.
    fn get_drop_fn(&self) -> unsafe extern "C" fn(*mut v8::ffi::Isolate, *mut c_void);

    /// Returns mutable access to the `ResourceState` tracking this resource's V8 wrapper.
    fn get_state(&mut self) -> &mut ResourceState;
}

/// Caches the V8 `FunctionTemplate` for a resource type.
///
/// A `ResourceTemplate` is created per Lock and caches the V8 function template used to
/// instantiate JavaScript wrappers for a resource type. This avoids recreating the template
/// on every wrap operation.
pub trait ResourceTemplate {
    /// Creates a new template for the given lock, initializing the V8 function template.
    fn new(lock: &mut Lock) -> Self;

    /// Returns the cached V8 function template used to create wrappers.
    fn get_constructor(&self) -> &v8::Global<v8::FunctionTemplate>;
}

/// Rust types that are deep-copied into JavaScript as value types.
///
/// Unlike resource types, struct types are copied entirely into JavaScript objects with no
/// further Rust involvement after wrapping. This is analogous to `JSG_STRUCT` in C++ JSG.
pub trait Struct: Type {}

/// Drops a resource by reconstructing it from a raw pointer and dropping it.
/// This function is typically used as a callback when V8 garbage collects a wrapped object.
///
/// # Safety
/// The caller must ensure:
/// - `this` is a valid pointer to a resource of type `R` that was previously created by `Ref::into_raw()`
/// - This function is only called once per resource
/// - The resource has not already been dropped
pub unsafe fn drop_resource<R: Resource>(_isolate: *mut ffi::Isolate, this: *mut c_void) {
    let this = this.cast::<R>();
    let this = unsafe { Ref::from_raw(this) };
    drop(this);
}

/// Tracks the lifetime of Rust resources exposed to a V8 context.
///
/// When Rust resources are wrapped for JavaScript, they are leaked via `Ref::into_raw()` to
/// create stable pointers that outlive V8's GC. The Realm tracks all such leaked resources
/// and ensures deterministic cleanup when the V8 context is disposed.
///
/// A Realm is created per V8 context and stored in the context's embedder data. Resources
/// register themselves via `add_resource()` during wrapping. When the context is torn down,
/// `Drop` iterates all tracked resources and calls their drop functions to reconstruct and
/// free any leaked `Ref<R>` values for wrappers not yet collected by V8's GC.
pub struct Realm {
    isolate: v8::IsolatePtr,
    resources: Vec<*mut ResourceState>,
}

impl Realm {
    /// Creates a new Realm from a V8 isolate.
    pub fn from_isolate(isolate: v8::IsolatePtr) -> Self {
        Self {
            isolate,
            resources: Vec::new(),
        }
    }

    pub fn add_resource(&mut self, resource: NonNull<ResourceState>) {
        self.resources.push(resource.as_ptr());
    }

    pub fn isolate(&self) -> v8::IsolatePtr {
        self.isolate
    }
}

impl Drop for Realm {
    fn drop(&mut self) {
        debug_assert!(
            unsafe { self.isolate.is_locked() },
            "Realm must be dropped while holding the isolate lock"
        );

        // Clean up all leaked Refs during deterministic context disposal.
        // Each resource_ptr points to a ResourceState embedded in a Resource.
        // When wrapping a resource, we leak a Ref<R> and store the raw pointer
        // in ResourceState.this. If strong_wrapper is still Some, the V8 object
        // wasn't GC'd, so we must manually drop the leaked Ref by calling drop_fn.
        for resource_ptr in &self.resources {
            unsafe {
                let resource_state = &**resource_ptr;
                // Only drop if the wrapper hasn't been collected by V8's GC
                if resource_state.strong_wrapper.is_some()
                    && let Some(drop_fn) = resource_state.drop_fn
                    && !resource_state.this.is_null()
                {
                    // Note: Do not access resource_state after drop_fn returns, as
                    // ResourceState is embedded inside the Resource which is now freed.
                    let isolate = resource_state.isolate.expect("isolate should be set");
                    drop_fn(isolate.as_ffi(), resource_state.this);
                }
            }
        }
        self.resources.clear();
    }
}

#[expect(clippy::unnecessary_box_returns)]
unsafe fn realm_create(isolate: *mut v8::ffi::Isolate) -> Box<Realm> {
    unsafe { Box::new(Realm::from_isolate(v8::IsolatePtr::from_ffi(isolate))) }
}
