use core::ffi::c_void;
use std::marker::PhantomData;

use crate::Lock;

#[expect(clippy::missing_safety_doc)]
#[cxx::bridge(namespace = "workerd::rust::jsg")]
pub mod ffi {
    #[derive(Debug)]
    struct Local {
        ptr: usize,
    }

    #[derive(Debug)]
    struct Global {
        ptr: usize,
    }

    enum ExceptionType {
        RangeError,
        ReferenceError,
        SyntaxError,
        TypeError,
        Error,
    }

    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type Isolate;
        type FunctionCallbackInfo;

        // Local<T>
        pub unsafe fn local_drop(value: Local);
        pub unsafe fn local_clone(value: &Local) -> Local;
        pub unsafe fn local_to_global(isolate: *mut Isolate, value: Local) -> Global;
        pub unsafe fn local_new_number(isolate: *mut Isolate, value: f64) -> Local;
        pub unsafe fn local_new_string(isolate: *mut Isolate, value: &str) -> Local;
        pub unsafe fn local_new_object(isolate: *mut Isolate) -> Local;
        pub unsafe fn local_eq(lhs: &Local, rhs: &Local) -> bool;
        pub unsafe fn local_has_value(value: &Local) -> bool;
        pub unsafe fn local_is_string(value: &Local) -> bool;

        // Local<Object>
        pub unsafe fn local_object_set_property(
            isolate: *mut Isolate,
            object: &mut Local,
            key: &str,
            value: Local,
        );
        pub unsafe fn local_object_has_property(
            isolate: *mut Isolate,
            object: &Local,
            key: &str,
        ) -> bool;
        pub unsafe fn local_object_get_property(
            isolate: *mut Isolate,
            object: &Local,
            key: &str,
        ) -> KjMaybe<Local>;

        // Global<T>
        pub unsafe fn global_drop(value: Global);
        pub unsafe fn global_clone(value: &Global) -> Global;
        pub unsafe fn global_to_local(isolate: *mut Isolate, value: &Global) -> Local;
        pub unsafe fn global_make_weak(
            isolate: *mut Isolate,
            value: *mut Global,
            data: usize, /* void* */
            callback: unsafe fn(isolate: *mut Isolate, data: usize) -> (),
        );

        // Unwrappers
        pub unsafe fn unwrap_string(isolate: *mut Isolate, value: Local) -> String;

        // FunctionCallbackInfo
        pub unsafe fn fci_get_isolate(args: *mut FunctionCallbackInfo) -> *mut Isolate;
        pub unsafe fn fci_get_this(args: *mut FunctionCallbackInfo) -> Local;
        pub unsafe fn fci_get_length(args: *mut FunctionCallbackInfo) -> usize;
        pub unsafe fn fci_get_arg(args: *mut FunctionCallbackInfo, index: usize) -> Local;
        pub unsafe fn fci_set_return_value(args: *mut FunctionCallbackInfo, value: Local);

        // Errors
        pub unsafe fn exception_create(
            isolate: *mut Isolate,
            exception_type: ExceptionType,
            message: &str,
        ) -> Local;

        // Isolate
        pub unsafe fn isolate_throw_exception(isolate: *mut Isolate, exception: Local);
        pub unsafe fn isolate_throw_error(isolate: *mut Isolate, message: &str);
        pub unsafe fn isolate_is_locked(isolate: *mut Isolate) -> bool;
    }

    pub struct ConstructorDescriptor {
        callback: usize,
    }

    pub struct MethodDescriptor {
        name: String,
        callback: usize,
    }

    pub struct StaticMethodDescriptor {
        name: String,
        callback: usize,
    }

    pub struct ResourceDescriptor {
        pub name: String,
        pub constructor: KjMaybe<ConstructorDescriptor>,
        pub methods: Vec<MethodDescriptor>,
        pub static_methods: Vec<StaticMethodDescriptor>,
    }

    // Resources
    unsafe extern "C++" {
        unsafe fn create_resource_template(
            isolate: *mut Isolate,
            descriptor: &ResourceDescriptor,
        ) -> Global /* v8::Global<FunctionTemplate> */;

        pub unsafe fn wrap_resource(
            isolate: *mut Isolate,
            resource: usize,      /* R* */
            constructor: &Global, /* v8::Global<FunctionTemplate> */
            drop_callback: usize, /* R* -> () */
        ) -> Local /* v8::Local<Value> */;

        pub unsafe fn unwrap_resource(
            isolate: *mut Isolate,
            value: Local, /* v8::LocalValue */
        ) -> usize /* R* */;
    }

    unsafe extern "C++" {
        type ModuleRegistry;

        pub unsafe fn register_add_builtin_module(
            registry: Pin<&mut ModuleRegistry>,
            specifier: &str,
            callback: unsafe fn(*mut Isolate) -> Local,
        );
    }
}

impl std::fmt::Display for ffi::ExceptionType {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let name = match *self {
            Self::RangeError => "RangeError",
            Self::ReferenceError => "ReferenceError",
            Self::SyntaxError => "SyntaxError",
            Self::TypeError => "TypeError",
            Self::Error => "Error",
            _ => unreachable!(),
        };
        write!(f, "{name}")
    }
}

// Marker types for Local<T>
#[derive(Debug)]
pub struct Value;
#[derive(Debug)]
pub struct Object;
pub struct FunctionTemplate;

// Generic Local<'a, T> handle with lifetime
#[derive(Debug)]
pub struct Local<'a, T> {
    handle: ffi::Local,
    isolate: *mut ffi::Isolate,
    _marker: PhantomData<(&'a (), T)>,
}

impl<T> Drop for Local<'_, T> {
    fn drop(&mut self) {
        if self.handle.ptr == 0 {
            return;
        }
        let handle = std::mem::replace(&mut self.handle, ffi::Local { ptr: 0 });
        unsafe { ffi::local_drop(handle) };
    }
}

// Common implementations for all Local<'a, T>
#[expect(clippy::elidable_lifetime_names)]
impl<'a, T> Local<'a, T> {
    /// Creates a `Local` from an FFI handle.
    ///
    /// # Safety
    /// The caller must ensure that `isolate` is valid and that `handle` points to a V8 value
    /// that is still alive within the current `HandleScope`.
    pub unsafe fn from_ffi(isolate: *mut ffi::Isolate, handle: ffi::Local) -> Self {
        Local {
            handle,
            isolate,
            _marker: PhantomData,
        }
    }

    /// Consumes this `Local` and returns the underlying FFI handle.
    ///
    /// # Safety
    /// The returned FFI handle must not outlive the `HandleScope` that owns the V8 value.
    pub unsafe fn into_ffi(mut self) -> ffi::Local {
        let handle = ffi::Local {
            ptr: self.handle.ptr,
        };
        self.handle.ptr = 0;
        handle
    }

    /// Returns a reference to the underlying FFI handle.
    ///
    /// # Safety
    /// The caller must ensure the returned reference is not used after this `Local` is dropped.
    pub unsafe fn as_ffi_ref(&self) -> &ffi::Local {
        &self.handle
    }

    pub fn has_value(&self) -> bool {
        unsafe { ffi::local_has_value(&self.handle) }
    }

    pub fn is_string(&self) -> bool {
        unsafe { ffi::local_is_string(&self.handle) }
    }
}

impl<T> Clone for Local<'_, T> {
    fn clone(&self) -> Self {
        unsafe { Self::from_ffi(self.isolate, ffi::local_clone(&self.handle)) }
    }
}

// Value-specific implementations
impl<'a> Local<'a, Value> {
    pub fn to_global(self, lock: &'a mut Lock) -> Global<Value> {
        unsafe { ffi::local_to_global(lock.isolate(), self.into_ffi()).into() }
    }
}

impl PartialEq for Local<'_, Value> {
    fn eq(&self, other: &Self) -> bool {
        unsafe { ffi::local_eq(&self.handle, &other.handle) }
    }
}

impl<'a> From<Local<'a, Object>> for Local<'a, Value> {
    fn from(value: Local<'a, Object>) -> Self {
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

// TODO: We need to figure out a smart way of avoiding duplication.
impl<'a> From<Local<'a, FunctionTemplate>> for Local<'a, Value> {
    fn from(value: Local<'a, FunctionTemplate>) -> Self {
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

// Object-specific implementations
impl<'a> Local<'a, Object> {
    pub fn set(&mut self, lock: &mut Lock, key: &str, value: Local<'a, Value>) {
        unsafe {
            ffi::local_object_set_property(lock.isolate(), &mut self.handle, key, value.into_ffi());
        }
    }

    pub fn has(&self, lock: &mut Lock, key: &str) -> bool {
        unsafe { ffi::local_object_has_property(lock.isolate(), &self.handle, key) }
    }

    pub fn get(&self, lock: &mut Lock, key: &str) -> Option<Local<'a, Value>> {
        if !self.has(lock, key) {
            return None;
        }

        unsafe {
            let maybe_local = ffi::local_object_get_property(lock.isolate(), &self.handle, key);
            let opt_local: Option<ffi::Local> = maybe_local.into();
            opt_local.map(|local| Local::from_ffi(lock.isolate(), local))
        }
    }
}

impl<'a> From<Local<'a, Value>> for Local<'a, Object> {
    fn from(value: Local<'a, Value>) -> Self {
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

// Generic Global<T> handle without lifetime
pub struct Global<T> {
    handle: ffi::Global,
    _marker: PhantomData<T>,
}

// Common implementations for all Global<T>
impl<T> Global<T> {
    /// Creates a `Global` from an FFI handle.
    ///
    /// # Safety
    /// The caller must ensure that `handle` points to a valid V8 persistent handle.
    pub unsafe fn from_ffi(handle: ffi::Global) -> Self {
        Self {
            handle,
            _marker: PhantomData,
        }
    }

    /// Returns a reference to the underlying FFI handle.
    ///
    /// # Safety
    /// The caller must ensure the returned reference is not used after this `Global` is dropped.
    pub unsafe fn as_ffi_ref(&self) -> &ffi::Global {
        &self.handle
    }

    pub fn as_local<'a>(&self, lock: &mut Lock) -> Local<'a, FunctionTemplate> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::global_to_local(lock.isolate(), &self.handle),
            )
        }
    }

    /// Makes this global handle weak, allowing V8 to garbage collect the object
    /// and invoke the callback when the object is being collected.
    ///
    /// # Safety
    /// The caller must ensure:
    /// - `isolate` is a valid pointer to a V8 isolate
    /// - `data` encodes a value that remains valid until the callback is invoked
    /// - `callback` can safely handle the provided data value
    pub unsafe fn make_weak(
        &mut self,
        isolate: *mut ffi::Isolate,
        data: *mut c_void,
        callback: fn(*mut ffi::Isolate, usize) -> (),
    ) {
        unsafe {
            ffi::global_make_weak(isolate, &raw mut self.handle, data as usize, callback);
        }
    }
}

impl<T> From<Local<'_, T>> for Global<T> {
    fn from(local: Local<'_, T>) -> Self {
        Self {
            handle: unsafe { ffi::local_to_global(local.isolate, local.into_ffi()) },
            _marker: PhantomData,
        }
    }
}

// Allow implicit conversion from ffi::Global
impl<T> From<ffi::Global> for Global<T> {
    fn from(handle: ffi::Global) -> Self {
        Self {
            handle,
            _marker: PhantomData,
        }
    }
}

impl<T> Drop for Global<T> {
    fn drop(&mut self) {
        let handle = ffi::Global {
            ptr: self.handle.ptr,
        };
        unsafe {
            ffi::global_drop(handle);
        }
    }
}

impl<T> Clone for Global<T> {
    fn clone(&self) -> Self {
        unsafe { ffi::global_clone(&self.handle).into() }
    }
}

pub trait ToLocalValue {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value>;
}

impl ToLocalValue for u8 {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_new_number(lock.isolate(), f64::from(*self)),
            )
        }
    }
}

impl ToLocalValue for u32 {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_new_number(lock.isolate(), f64::from(*self)),
            )
        }
    }
}

impl ToLocalValue for String {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        self.as_str().to_local(lock)
    }
}

impl ToLocalValue for &str {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        unsafe { Local::from_ffi(lock.isolate(), ffi::local_new_string(lock.isolate(), self)) }
    }
}

pub struct FunctionCallbackInfo<'a>(*mut ffi::FunctionCallbackInfo, PhantomData<&'a ()>);

impl<'a> FunctionCallbackInfo<'a> {
    /// # Safety
    /// The caller must ensure that `info` is a valid pointer to `FunctionCallbackInfo`.
    pub unsafe fn from_ffi(info: *mut ffi::FunctionCallbackInfo) -> Self {
        Self(info, PhantomData)
    }

    /// Returns the V8 isolate associated with this function callback.
    ///
    /// # Safety
    /// The returned isolate pointer must not be used after the callback returns.
    pub unsafe fn isolate(&self) -> *mut ffi::Isolate {
        unsafe { ffi::fci_get_isolate(self.0) }
    }

    pub fn this(&self) -> Local<'a, Value> {
        unsafe { Local::from_ffi(self.isolate(), ffi::fci_get_this(self.0)) }
    }

    pub fn len(&self) -> usize {
        unsafe { ffi::fci_get_length(self.0) }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn get(&self, index: usize) -> Local<'a, Value> {
        debug_assert!(index <= self.len());
        unsafe { Local::from_ffi(self.isolate(), ffi::fci_get_arg(self.0, index)) }
    }

    pub fn set_return_value(&self, value: Local<Value>) {
        unsafe {
            ffi::fci_set_return_value(self.0, value.into_ffi());
        }
    }
}
