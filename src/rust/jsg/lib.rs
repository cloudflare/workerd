use std::any::Any;
use std::any::TypeId;
use std::collections::HashMap;
use std::future::Future;
use std::num::ParseIntError;

use kj_rs::KjMaybe;

pub mod modules;
pub mod resource;
pub mod v8;
pub use resource::Ref;
pub use resource::ResourceImpl;
pub use resource::WeakRef;
pub use v8::GarbageCollected;
pub use v8::GcVisitor;
pub use v8::Isolate;
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
        name: R::class_name().into(),
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
                    name: name.to_owned(),
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
                        name: name.to_owned(),
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
        v8::ffi::create_resource_template(lock.isolate().as_ptr(), &get_resource_descriptor::<R>())
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

    /// # Safety
    /// The caller must ensure the isolate is valid and that the exception is thrown within the
    /// correct isolate/context.
    pub unsafe fn as_exception<'a>(&self, isolate: v8::Isolate) -> v8::Local<'a, v8::Value> {
        unsafe {
            v8::Local::from_ffi(
                isolate,
                v8::ffi::exception_create(isolate.as_ptr(), self.name, &self.message),
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

/// Provides access to V8 operations within an isolate lock.
///
/// A Lock wraps a V8 isolate pointer and is passed to resource methods and callbacks to
/// perform V8 operations like creating objects, wrapping values, and accessing the Realm.
/// This is analogous to `jsg::Lock` in C++ JSG.
pub struct Lock {
    isolate: v8::Isolate,
}

impl Lock {
    /// # Safety
    /// The caller must ensure that `args` is a valid pointer to `FunctionCallbackInfo`.
    pub unsafe fn from_args(args: *mut v8::ffi::FunctionCallbackInfo) -> Self {
        // SAFETY: fci_get_isolate returns a valid, non-null isolate pointer
        unsafe { Self::from_isolate(v8::Isolate::from_raw(v8::ffi::fci_get_isolate(args))) }
    }

    /// Creates a Lock from an Isolate.
    pub fn from_isolate(isolate: v8::Isolate) -> Self {
        Self { isolate }
    }

    /// Returns the isolate.
    pub fn isolate(&self) -> v8::Isolate {
        self.isolate
    }

    pub fn is_locked(&self) -> bool {
        self.isolate.is_locked()
    }

    pub fn new_object<'a>(&mut self) -> v8::Local<'a, v8::Object> {
        // SAFETY: Lock guarantees the isolate is valid and locked
        unsafe {
            v8::Local::from_ffi(
                self.isolate(),
                v8::ffi::local_new_object(self.isolate().as_ptr()),
            )
        }
    }

    pub fn throw_error(&mut self, message: &str) {
        // SAFETY: Lock guarantees the isolate is valid and locked
        unsafe { v8::ffi::isolate_throw_error(self.isolate().as_ptr(), message) }
    }

    /// Allocates a `RustResource` on the cppgc heap.
    ///
    /// # Safety
    /// The data must contain valid pointers to drop and trace functions.
    pub unsafe fn alloc(&mut self, data: v8::ffi::RustResourceData) -> *mut v8::ffi::RustResource {
        unsafe { v8::ffi::cppgc_allocate(self.isolate().as_ptr(), data) }
    }

    pub fn await_io<F, C, I, R>(self, _fut: F, _callback: C) -> Result<R>
    where
        F: Future<Output = I>,
        C: FnOnce(Self, I) -> Result<R>,
    {
        todo!()
    }

    pub fn realm<'a, 'b>(&'a mut self) -> &'b mut Realm
    where
        'b: 'a,
    {
        // SAFETY: realm_from_isolate returns a valid pointer to the Realm stored
        // in the isolate's data slot, and Lock guarantees the isolate is locked
        unsafe { &mut *crate::ffi::realm_from_isolate(self.isolate.as_ptr()) }
    }
}

/// TODO: Implement `memory_info(jsg::MemoryTracker)`
pub trait Type {
    type This;

    fn class_name() -> &'static str;
    /// Same as jsgGetMemoryName
    fn memory_name() -> &'static str {
        std::any::type_name::<Self>()
    }
    /// Same as jsgGetMemorySelfSize
    fn memory_self_size() -> usize
    where
        Self: Sized,
    {
        std::mem::size_of::<Self>()
    }
    /// Wraps this struct as a JavaScript value by deep-copying its fields.
    fn wrap<'a, 'b>(this: Self::This, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a;

    fn unwrap(lock: &mut Lock, value: v8::Local<v8::Value>) -> Self::This;
}

pub enum Member {
    Constructor {
        callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
    Method {
        name: &'static str,
        callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
    Property {
        name: &'static str,
        getter_callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
        setter_callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
    StaticMethod {
        name: &'static str,
        callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
}

/// Rust types exposed to JavaScript as resource types.
///
/// Resource types are passed by reference and call back into Rust when JavaScript accesses
/// their members. This is analogous to `JSG_RESOURCE_TYPE` in C++ JSG. Resources must provide
/// member declarations, a cleanup function for GC, and access to their V8 wrapper state.
pub trait Resource: Type + GarbageCollected + Sized {
    type Template: ResourceTemplate;

    /// Returns the list of methods, properties, and constructors exposed to JavaScript.
    fn members() -> Vec<Member>
    where
        Self: Sized;

    /// Allocates a resource instance and returns a reference-counted handle.
    ///
    /// The returned `Ref<Self>` manages the resource's lifetime. When all `Ref` handles are
    /// dropped and a JavaScript wrapper exists, the instance becomes weak and eligible for
    /// garbage collection.
    fn alloc(_lock: &mut Lock, this: Self) -> Ref<Self> {
        resource::Instance::new(this)
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
    isolate: v8::Isolate,
    /// Resource templates keyed by `TypeId`.
    templates: HashMap<TypeId, Box<dyn Any>>,
}

impl Realm {
    /// Creates a new Realm for the given isolate.
    ///
    /// # Safety
    /// The caller must ensure that `isolate` is a valid, non-null pointer.
    pub unsafe fn new(isolate: *mut v8::ffi::Isolate) -> Self {
        Self {
            // SAFETY: Caller guarantees isolate is non-null
            isolate: unsafe { v8::Isolate::from_raw(isolate) },
            templates: HashMap::default(),
        }
    }

    /// Returns the isolate.
    pub fn isolate(&self) -> v8::Isolate {
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
            self.isolate.is_locked(),
            "Realm must be dropped while holding the isolate lock"
        );
    }
}

#[expect(clippy::unnecessary_box_returns)]
unsafe fn realm_create(isolate: *mut v8::ffi::Isolate) -> Box<Realm> {
    // SAFETY: Caller guarantees isolate is valid and non-null
    Box::new(unsafe { Realm::new(isolate) })
}

/// Handles a result by setting the return value or throwing an error.
///
/// # Safety
/// The caller must ensure V8 operations are performed within the correct isolate/context.
pub unsafe fn handle_result<T: Type, E: std::fmt::Display>(
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
        "String"
    }

    fn wrap<'a, 'b>(this: Self::This, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        this.to_local(lock)
    }

    fn unwrap(lock: &mut Lock, value: v8::Local<v8::Value>) -> Self::This {
        // SAFETY: Lock guarantees isolate is valid; value is consumed and converted to FFI handle
        unsafe { v8::ffi::unwrap_string(lock.isolate().as_ptr(), value.into_ffi()) }
    }
}
