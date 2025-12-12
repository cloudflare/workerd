use std::future::Future;
use std::num::ParseIntError;

use kj_rs::KjMaybe;

pub mod modules;
pub mod resource;
pub mod v8;
pub use resource::Ref;
pub use resource::ResourceImpl;
pub use resource::Resources;
pub use v8::ffi::ExceptionType;

use crate::v8::ToLocalValue;

#[cxx::bridge(namespace = "workerd::rust::jsg")]
mod ffi {
    extern "Rust" {
        type Realm;
        #[expect(clippy::unnecessary_box_returns)]
        unsafe fn realm_create(isolate: *mut Isolate) -> Box<Realm>;

        /// Called from C++ weak callback to invoke the drop function stored in State.
        /// The `state` must point to a valid `resource::State` struct.
        unsafe fn invoke_weak_drop(state: usize);
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
    unsafe {
        v8::ffi::create_resource_template(lock.isolate(), &get_resource_descriptor::<R>()).into()
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
    pub unsafe fn as_exception<'a>(
        &self,
        isolate: *mut v8::ffi::Isolate,
    ) -> v8::Local<'a, v8::Value> {
        unsafe {
            v8::Local::from_ffi(
                isolate,
                v8::ffi::exception_create(isolate, self.name, &self.message),
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
    isolate: *mut v8::ffi::Isolate,
}

impl Lock {
    /// # Safety
    /// The caller must ensure that `args` is a valid pointer to `FunctionCallbackInfo`.
    pub unsafe fn from_args(args: *mut v8::ffi::FunctionCallbackInfo) -> Self {
        unsafe { Self::from_isolate(v8::ffi::fci_get_isolate(args)) }
    }

    /// # Safety
    /// The caller must ensure that `isolate` is a valid pointer to an `Isolate`.
    pub unsafe fn from_isolate(isolate: *mut v8::ffi::Isolate) -> Self {
        Self { isolate }
    }

    /// Returns a raw pointer to the isolate.
    ///
    /// # Safety
    /// The caller must ensure the pointer is not used after the Lock is dropped and that all
    /// V8 API calls using this pointer are made while holding the isolate lock.
    pub unsafe fn isolate(&mut self) -> *mut v8::ffi::Isolate {
        self.isolate
    }

    pub fn new_object<'a>(&mut self) -> v8::Local<'a, v8::Object> {
        unsafe { v8::Local::from_ffi(self.isolate(), v8::ffi::local_new_object(self.isolate())) }
    }

    pub fn await_io<F, C, I, R>(self, _fut: F, _callback: C) -> Result<R>
    where
        F: Future<Output = I>,
        C: FnOnce(Self, I) -> Result<R>,
    {
        todo!()
    }

    pub fn get_realm<'a, 'b>(&'a mut self) -> &'b mut Realm
    where
        'b: 'a,
    {
        unsafe { &mut *crate::ffi::realm_from_isolate(self.isolate()) }
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
pub trait Resource: Type + Sized {
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
/// A `ResourceTemplate` is created per Lock and caches the V8 function template used to
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

/// Per-context state for Rust resources exposed to JavaScript.
///
/// A Realm is created for each V8 context and stored in the context's embedder data. It holds
/// cached function templates and tracks resource instances. When all Rust `Ref` handles to a
/// resource are dropped, the V8 wrapper becomes weak and V8 GC will trigger cleanup via the
/// weak callback.
pub struct Realm {
    isolate: *mut v8::ffi::Isolate,
    pub resources: Resources,
}

impl Realm {
    pub fn new(isolate: *mut v8::ffi::Isolate) -> Self {
        Self {
            isolate,
            resources: Resources::default(),
        }
    }

    pub fn isolate(&self) -> *mut v8::ffi::Isolate {
        self.isolate
    }

    pub fn get_resources<R: Resource + 'static>(&mut self) -> &mut ResourceImpl<R>
    where
        <R as Resource>::Template: 'static,
    {
        let mut lock = unsafe { Lock::from_isolate(self.isolate) };
        self.resources.get_or_create::<R>(&mut lock)
    }
}

impl Drop for Realm {
    fn drop(&mut self) {
        debug_assert!(
            unsafe { v8::ffi::isolate_is_locked(self.isolate) },
            "Realm must be dropped while holding the isolate lock"
        );
    }
}

#[expect(clippy::unnecessary_box_returns)]
unsafe fn realm_create(isolate: *mut v8::ffi::Isolate) -> Box<Realm> {
    Box::new(Realm::new(isolate))
}

/// Called from C++ weak callback to invoke the drop function stored in State.
///
/// # Safety
/// The `state` must point to a valid `resource::State` struct with `drop_fn` set.
/// The `State::this` pointer must still be valid.
unsafe fn invoke_weak_drop(state: usize) {
    let state_ptr = state as *mut resource::State;
    let state = unsafe { &*state_ptr };

    // The drop_fn must be set - it's always initialized in State::new()
    let drop_fn = state
        .drop_fn()
        .expect("drop_fn must be set when invoke_weak_drop is called");

    // The this_ptr must be set - it's set when the resource is wrapped
    let this_ptr = state
        .this_ptr()
        .expect("this_ptr must be set when invoke_weak_drop is called");

    // Call the drop function with the instance pointer
    unsafe { drop_fn(this_ptr.as_ptr()) };
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
            let description = err.to_string();
            unsafe { v8::ffi::isolate_throw_error(lock.isolate(), &description) };
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
        unsafe { v8::ffi::unwrap_string(lock.isolate(), value.into_ffi()) }
    }
}
