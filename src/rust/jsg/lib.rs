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
pub use resource::Ref;
pub use resource::ResourceImpl;
pub use resource::WeakRef;
pub use v8::GarbageCollected;
pub use v8::GcParent;
pub use v8::GcVisitor;
pub use v8::IsolatePtr;
pub use v8::TracedReference;
pub use v8::cppgc;
pub use v8::ffi::ExceptionType;

use crate::v8::ToLocalValue;

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

pub struct Error {
    pub name: v8::ffi::ExceptionType,
    pub message: String,
}

impl Error {
    pub fn new(name: v8::ffi::ExceptionType, message: String) -> Self {
        Self { name, message }
    }

    /// Creates a V8 exception from this error.
    pub fn as_exception<'a>(&self, isolate: v8::IsolatePtr) -> v8::Local<'a, v8::Value> {
        unsafe {
            v8::Local::from_ffi(
                isolate,
                v8::ffi::exception_create(isolate.as_ffi(), self.name, &self.message),
            )
        }
    }
}

impl Default for Error {
    fn default() -> Self {
        Self {
            name: v8::ffi::ExceptionType::Error,
            message: "An unknown error occurred".to_owned(),
        }
    }
}

impl From<ParseIntError> for Error {
    fn from(err: ParseIntError) -> Self {
        Self::new(
            v8::ffi::ExceptionType::TypeError,
            format!("Failed to parse integer: {err}"),
        )
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
pub struct NonCoercible<T: Type> {
    value: T,
}

impl<T: Type> NonCoercible<T> {
    /// Creates a new `NonCoercible` wrapper around the given value.
    pub fn new(value: T) -> Self {
        Self { value }
    }

    /// Unwraps a V8 value into `NonCoercible<T>`, throwing a JavaScript error if the value
    /// is not exactly the expected type.
    ///
    /// Returns `Some(NonCoercible<T>)` if the value is the exact type, or `None` if an error
    /// was thrown (in which case the caller should return early).
    ///
    /// # Safety
    /// The caller must ensure `lock` is valid and `value` is a valid V8 local handle.
    pub unsafe fn unwrap(lock: &mut Lock, value: v8::Local<v8::Value>) -> Option<Self> {
        if !T::is_exact(&value) {
            let type_name = T::class_name();
            let error_msg = format!("Expected a {} value but got {}", type_name, value.type_of());
            unsafe { v8::ffi::isolate_throw_error(lock.isolate().as_ffi(), &error_msg) };
            return None;
        }
        let inner = T::unwrap(lock.isolate(), value);
        Some(Self::new(inner))
    }
}

impl<T: Type> From<T> for NonCoercible<T> {
    fn from(value: T) -> Self {
        Self::new(value)
    }
}

impl<T: Type> AsRef<T> for NonCoercible<T> {
    fn as_ref(&self) -> &T {
        &self.value
    }
}

impl<T: Type> Deref for NonCoercible<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        &self.value
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
}

/// TODO: Implement `memory_info(jsg::MemoryTracker)`
pub trait Type: Sized {
    /// The input type for [`wrap()`](Self::wrap). For primitive types this is typically `Self`,
    /// but resource types may use `Ref<Self>` or other wrapper types.
    type This;

    fn class_name() -> &'static str;
    /// Same as jsgGetMemoryName
    fn memory_name() -> &'static str {
        std::any::type_name::<Self>()
    }
    /// Same as jsgGetMemorySelfSize
    fn memory_self_size() -> usize {
        std::mem::size_of::<Self>()
    }
    /// Wraps this struct as a JavaScript value by deep-copying its fields.
    fn wrap<'a, 'b>(this: Self::This, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a;

    /// Returns true if the V8 value is exactly this type (no coercion).
    /// Used by `NonCoercible<T>` to reject values that would require coercion.
    fn is_exact(value: &v8::Local<v8::Value>) -> bool;

    /// Unwraps a V8 value into this type without coercion.
    fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self;
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

/// Handles a result by setting the return value or throwing an error.
///
/// # Safety
/// The caller must ensure V8 operations are performed within the correct isolate/context.
pub unsafe fn handle_result<T: Type<This = T>, E: std::fmt::Display>(
    lock: &mut Lock,
    args: &mut v8::FunctionCallbackInfo,
    result: Result<T::This, E>,
) {
    match result {
        Ok(result) => args.set_return_value(T::wrap(result, lock)),
        Err(err) => {
            // TODO(soon): Make sure to use jsg::Error trait here and dynamically call proper method to throw the error.
            lock.throw_error(&err.to_string());
        }
    }
}

impl Type for String {
    type This = Self;

    fn class_name() -> &'static str {
        "string"
    }

    fn wrap<'a, 'b>(this: Self::This, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        this.to_local(lock)
    }

    fn is_exact(value: &v8::Local<v8::Value>) -> bool {
        value.is_string()
    }

    fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self {
        unsafe { v8::ffi::unwrap_string(isolate.as_ffi(), value.into_ffi()) }
    }
}

impl Type for bool {
    type This = Self;

    fn class_name() -> &'static str {
        "boolean"
    }

    fn wrap<'a, 'b>(this: Self::This, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        this.to_local(lock)
    }

    fn is_exact(value: &v8::Local<v8::Value>) -> bool {
        value.is_boolean()
    }

    fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self {
        unsafe { v8::ffi::unwrap_boolean(isolate.as_ffi(), value.into_ffi()) }
    }
}

impl Type for f64 {
    type This = Self;

    fn class_name() -> &'static str {
        "number"
    }

    fn wrap<'a, 'b>(this: Self::This, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        this.to_local(lock)
    }

    fn is_exact(value: &v8::Local<v8::Value>) -> bool {
        value.is_number()
    }

    fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self {
        unsafe { v8::ffi::unwrap_number(isolate.as_ffi(), value.into_ffi()) }
    }
}
