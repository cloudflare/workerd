//! V8 JavaScript engine bindings and garbage collector integration.
//!
//! This module provides Rust wrappers for V8 types and integration with the C++ `Wrappable`
//! garbage collection system used by workerd.
//!
//! # Core Types
//!
//! - [`IsolatePtr`] - Safe wrapper around `v8::Isolate*`, the V8 runtime instance
//! - [`Local<'a, T>`] - Stack-allocated handle to a V8 value, tied to a `HandleScope`
//! - [`Global<T>`] - Persistent handle that outlives `HandleScope`s
//!
//! # Garbage Collection
//!
//! Rust resources integrate with V8's GC through the C++ `Wrappable` base class. Each Rust
//! resource is wrapped in `Rc<R>` with a `Wrappable` on the KJ heap that bridges to cppgc
//! via a `CppgcShim`. The Wrappable's `data[0..1]` stores a fat pointer to
//! `dyn GarbageCollected` — the data part is the `Rc::into_raw` pointer (pointing to `R`
//! inside the `Rc` allocation) and the vtable part carries `R`'s `GarbageCollected` impl.
//! On destruction, `wrappable_invoke_drop` reconstructs the `Rc` via `Rc::from_raw` and
//! drops it, which may drop the resource. The Rust `Ref<R>` smart pointer holds its own
//! `Rc<R>` plus a `WrappableRc` (`KjRc<Wrappable>`) for reference-counted ownership. On
//! drop, `wrappable_remove_strong_ref()` handles GC cleanup via `maybeDeferDestruction()`,
//! then the `WrappableRc` drop decrements the `kj::Rc` refcount.

use std::ffi::c_char;
use std::fmt::Display;
use std::marker::PhantomData;
use std::pin::Pin;
use std::ptr::NonNull;
use std::rc::Rc;

use crate::FromJS;
use crate::GarbageCollected;
use crate::Lock;
use crate::Number;
use crate::Resource;

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

    #[derive(Debug)]
    struct GcVisitor {
        ptr: usize,
    }

    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    pub enum ExceptionType {
        OperationError,
        DataError,
        DataCloneError,
        InvalidAccessError,
        InvalidStateError,
        InvalidCharacterError,
        NotSupportedError,
        SyntaxError,
        TimeoutError,
        TypeMismatchError,
        AbortError,
        NotFoundError,
        TypeError,
        Error,
        RangeError,
        ReferenceError,
    }

    extern "Rust" {
        /// Called from C++ Wrappable destructor to drop the Rust object.
        /// Reconstructs the `Rc<dyn GarbageCollected>` from `data[0..1]` and drops it.
        unsafe fn wrappable_invoke_drop(wrappable: Pin<&mut Wrappable>);

        /// Called from C++ Wrappable::jsgVisitForGc to trace nested handles.
        /// Reconstructs `&dyn GarbageCollected` from `data[0..1]` and calls `trace()`.
        unsafe fn wrappable_invoke_trace(wrappable: &Wrappable, visitor: *mut GcVisitor);

        /// Called from C++ Wrappable::jsgGetMemoryName.
        /// Returns null if no name is available, otherwise a static string.
        unsafe fn wrappable_invoke_get_name(wrappable: &Wrappable) -> *const c_char;
    }

    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type Isolate;
        type FunctionCallbackInfo;
        type Wrappable;

        // Local<T>
        pub unsafe fn local_drop(value: Local);
        pub unsafe fn local_clone(value: &Local) -> Local;
        pub unsafe fn local_to_global(isolate: *mut Isolate, value: Local) -> Global;
        pub unsafe fn local_new_number(isolate: *mut Isolate, value: f64) -> Local;
        pub unsafe fn local_new_string(isolate: *mut Isolate, value: &str) -> Local;
        pub unsafe fn local_new_boolean(isolate: *mut Isolate, value: bool) -> Local;
        pub unsafe fn local_new_object(isolate: *mut Isolate) -> Local;
        pub unsafe fn local_new_null(isolate: *mut Isolate) -> Local;
        pub unsafe fn local_new_undefined(isolate: *mut Isolate) -> Local;
        pub unsafe fn local_new_array(isolate: *mut Isolate, length: usize) -> Local;
        pub unsafe fn local_new_uint8_array(
            isolate: *mut Isolate,
            data: *const u8,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_uint16_array(
            isolate: *mut Isolate,
            data: *const u16,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_uint32_array(
            isolate: *mut Isolate,
            data: *const u32,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_int8_array(
            isolate: *mut Isolate,
            data: *const i8,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_int16_array(
            isolate: *mut Isolate,
            data: *const i16,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_int32_array(
            isolate: *mut Isolate,
            data: *const i32,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_float32_array(
            isolate: *mut Isolate,
            data: *const f32,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_float64_array(
            isolate: *mut Isolate,
            data: *const f64,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_bigint64_array(
            isolate: *mut Isolate,
            data: *const i64,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_biguint64_array(
            isolate: *mut Isolate,
            data: *const u64,
            length: usize,
        ) -> Local;
        pub unsafe fn local_eq(lhs: &Local, rhs: &Local) -> bool;
        pub unsafe fn local_has_value(value: &Local) -> bool;
        pub unsafe fn local_is_string(value: &Local) -> bool;
        pub unsafe fn local_is_boolean(value: &Local) -> bool;
        pub unsafe fn local_is_number(value: &Local) -> bool;
        pub unsafe fn local_is_null(value: &Local) -> bool;
        pub unsafe fn local_is_undefined(value: &Local) -> bool;
        pub unsafe fn local_is_null_or_undefined(value: &Local) -> bool;
        pub unsafe fn local_is_object(value: &Local) -> bool;
        pub unsafe fn local_is_native_error(value: &Local) -> bool;
        pub unsafe fn local_is_array(value: &Local) -> bool;
        pub unsafe fn local_is_uint8_array(value: &Local) -> bool;
        pub unsafe fn local_is_uint16_array(value: &Local) -> bool;
        pub unsafe fn local_is_uint32_array(value: &Local) -> bool;
        pub unsafe fn local_is_int8_array(value: &Local) -> bool;
        pub unsafe fn local_is_int16_array(value: &Local) -> bool;
        pub unsafe fn local_is_int32_array(value: &Local) -> bool;
        pub unsafe fn local_is_float32_array(value: &Local) -> bool;
        pub unsafe fn local_is_float64_array(value: &Local) -> bool;
        pub unsafe fn local_is_bigint64_array(value: &Local) -> bool;
        pub unsafe fn local_is_biguint64_array(value: &Local) -> bool;
        pub unsafe fn local_is_array_buffer(value: &Local) -> bool;
        pub unsafe fn local_is_array_buffer_view(value: &Local) -> bool;
        pub unsafe fn local_type_of(isolate: *mut Isolate, value: &Local) -> String;

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

        // Local<Array>
        pub unsafe fn local_array_length(isolate: *mut Isolate, array: &Local) -> u32;
        pub unsafe fn local_array_get(isolate: *mut Isolate, array: &Local, index: u32) -> Local;
        pub unsafe fn local_array_set(
            isolate: *mut Isolate,
            array: &mut Local,
            index: u32,
            value: Local,
        );
        pub unsafe fn local_array_iterate(isolate: *mut Isolate, value: Local) -> Vec<Global>;

        // Local<TypedArray>
        pub unsafe fn local_typed_array_length(isolate: *mut Isolate, array: &Local) -> usize;
        pub unsafe fn local_uint8_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> u8;
        pub unsafe fn local_uint16_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> u16;
        pub unsafe fn local_uint32_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> u32;
        pub unsafe fn local_int8_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> i8;
        pub unsafe fn local_int16_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> i16;
        pub unsafe fn local_int32_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> i32;
        pub unsafe fn local_float32_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> f32;
        pub unsafe fn local_float64_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> f64;
        pub unsafe fn local_bigint64_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> i64;
        pub unsafe fn local_biguint64_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> u64;

        // Global<T>
        pub unsafe fn global_reset(value: *mut Global);
        pub unsafe fn global_clone(value: &Global) -> Global;
        pub unsafe fn global_to_local(isolate: *mut Isolate, value: &Global) -> Local;

        // Wrappable - data access (used by wrappable_invoke_* callbacks)
        pub unsafe fn wrappable_data(wrappable: &Wrappable) -> *const usize;

        // Wrappable lifecycle — KjRc<Wrappable> for reference-counted ownership
        pub unsafe fn wrappable_new(data_ptr: usize, vtable_ptr: usize) -> KjRc<Wrappable>;

        pub unsafe fn wrappable_to_rc(wrappable: Pin<&mut Wrappable>) -> KjRc<Wrappable>;
        pub unsafe fn wrappable_add_strong_ref(wrappable: Pin<&mut Wrappable>);
        pub unsafe fn wrappable_remove_strong_ref(wrappable: Pin<&mut Wrappable>);
        pub unsafe fn wrappable_visit_ref(
            wrappable: Pin<&mut Wrappable>,
            ref_parent: *mut usize,
            ref_strong: *mut bool,
            visitor: *mut GcVisitor,
        );

        // Unwrappers
        pub unsafe fn unwrap_string(isolate: *mut Isolate, value: Local) -> String;
        pub unsafe fn unwrap_boolean(isolate: *mut Isolate, value: Local) -> bool;
        pub unsafe fn unwrap_number(isolate: *mut Isolate, value: Local) -> f64;
        pub unsafe fn unwrap_uint8_array(isolate: *mut Isolate, value: Local) -> Vec<u8>;
        pub unsafe fn unwrap_uint16_array(isolate: *mut Isolate, value: Local) -> Vec<u16>;
        pub unsafe fn unwrap_uint32_array(isolate: *mut Isolate, value: Local) -> Vec<u32>;
        pub unsafe fn unwrap_int8_array(isolate: *mut Isolate, value: Local) -> Vec<i8>;
        pub unsafe fn unwrap_int16_array(isolate: *mut Isolate, value: Local) -> Vec<i16>;
        pub unsafe fn unwrap_int32_array(isolate: *mut Isolate, value: Local) -> Vec<i32>;
        pub unsafe fn unwrap_float32_array(isolate: *mut Isolate, value: Local) -> Vec<f32>;
        pub unsafe fn unwrap_float64_array(isolate: *mut Isolate, value: Local) -> Vec<f64>;
        pub unsafe fn unwrap_bigint64_array(isolate: *mut Isolate, value: Local) -> Vec<i64>;
        pub unsafe fn unwrap_biguint64_array(isolate: *mut Isolate, value: Local) -> Vec<u64>;

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

    pub struct ResourceDescriptor {
        pub name: String,
        pub constructor: KjMaybe<ConstructorDescriptor>,
        pub methods: Vec<MethodDescriptor>,
        pub static_methods: Vec<MethodDescriptor>,
    }

    // Resources
    unsafe extern "C++" {
        unsafe fn create_resource_template(
            isolate: *mut Isolate,
            descriptor: &ResourceDescriptor,
        ) -> Global /* v8::Global<FunctionTemplate> */;

        pub unsafe fn wrap_resource(
            isolate: *mut Isolate,
            wrappable: KjRc<Wrappable>,
            constructor: &Global, /* v8::Global<FunctionTemplate> */
        ) -> Local /* v8::Local<Value> */;

        pub unsafe fn unwrap_resource(
            isolate: *mut Isolate,
            value: Local, /* v8::LocalValue */
        ) -> KjRc<Wrappable>;

        pub unsafe fn function_template_get_function(
            isolate: *mut Isolate,
            constructor: &Global, /* v8::Global<FunctionTemplate> */
        ) -> Local /* v8::Local<Function> */;
    }

    /// Module visibility level, mirroring workerd::jsg::ModuleType from modules.capnp.
    ///
    /// CXX shared enums cannot reference existing C++ enums, so we define matching values here.
    /// The conversion to workerd::jsg::ModuleType happens in jsg.h's RustModuleRegistry.
    enum ModuleType {
        Bundle = 0,
        Builtin = 1,
        Internal = 2,
    }

    unsafe extern "C++" {
        type ModuleRegistry;

        pub unsafe fn register_add_builtin_module(
            registry: Pin<&mut ModuleRegistry>,
            specifier: &str,
            callback: unsafe fn(*mut Isolate) -> Local,
            module_type: ModuleType,
        );
    }
}

impl std::fmt::Display for ffi::ExceptionType {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let name = match *self {
            Self::OperationError => "OperationError",
            Self::DataError => "DataError",
            Self::DataCloneError => "DataCloneError",
            Self::InvalidAccessError => "InvalidAccessError",
            Self::InvalidStateError => "InvalidStateError",
            Self::InvalidCharacterError => "InvalidCharacterError",
            Self::NotSupportedError => "NotSupportedError",
            Self::SyntaxError => "SyntaxError",
            Self::TimeoutError => "TimeoutError",
            Self::TypeMismatchError => "TypeMismatchError",
            Self::AbortError => "AbortError",
            Self::NotFoundError => "NotFoundError",
            Self::TypeError => "TypeError",
            Self::RangeError => "RangeError",
            Self::ReferenceError => "ReferenceError",
            _ => "Error",
        };
        write!(f, "{name}")
    }
}

// Marker types for Local<T>
#[derive(Debug)]
pub struct Value;

impl Display for Local<'_, Value> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let mut lock = unsafe { Lock::from_isolate_ptr(self.isolate.as_ffi()) };
        match String::from_js(&mut lock, self.clone()) {
            Ok(value) => write!(f, "{value}"),
            Err(e) => write!(f, "{e:?}"),
        }
    }
}
#[derive(Debug)]
pub struct Object;
pub struct Function;
pub struct FunctionTemplate;
pub struct Array;
pub struct TypedArray;
pub struct Uint8Array;
pub struct Uint16Array;
pub struct Uint32Array;
pub struct Int8Array;
pub struct Int16Array;
pub struct Int32Array;
pub struct Float32Array;
pub struct Float64Array;
pub struct BigInt64Array;
pub struct BigUint64Array;

// Generic Local<'a, T> handle with lifetime
#[derive(Debug)]
pub struct Local<'a, T> {
    handle: ffi::Local,
    isolate: IsolatePtr,
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
impl<'a, T> Local<'a, T> {
    /// Creates a `Local` from an FFI handle.
    ///
    /// # Safety
    /// The caller must ensure that `isolate` is valid and that `handle` points to a V8 value
    /// that is still alive within the current `HandleScope`.
    pub unsafe fn from_ffi(isolate: IsolatePtr, handle: ffi::Local) -> Self {
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
    pub unsafe fn as_ffi(&self) -> &ffi::Local {
        &self.handle
    }

    /// Returns a mutable reference to the underlying FFI handle.
    ///
    /// # Safety
    /// The caller must ensure the returned reference is not used after this `Local` is dropped.
    pub unsafe fn as_ffi_mut(&mut self) -> &mut ffi::Local {
        &mut self.handle
    }

    pub fn null(lock: &mut crate::Lock) -> Local<'a, Value> {
        unsafe { Local::from_ffi(lock.isolate(), ffi::local_new_null(lock.isolate().as_ffi())) }
    }

    pub fn undefined(lock: &mut crate::Lock) -> Local<'a, Value> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_new_undefined(lock.isolate().as_ffi()),
            )
        }
    }

    pub fn has_value(&self) -> bool {
        unsafe { ffi::local_has_value(&self.handle) }
    }

    pub fn is_string(&self) -> bool {
        unsafe { ffi::local_is_string(&self.handle) }
    }

    pub fn is_boolean(&self) -> bool {
        unsafe { ffi::local_is_boolean(&self.handle) }
    }

    pub fn is_number(&self) -> bool {
        unsafe { ffi::local_is_number(&self.handle) }
    }

    pub fn is_null(&self) -> bool {
        unsafe { ffi::local_is_null(&self.handle) }
    }

    pub fn is_undefined(&self) -> bool {
        unsafe { ffi::local_is_undefined(&self.handle) }
    }

    pub fn is_null_or_undefined(&self) -> bool {
        unsafe { ffi::local_is_null_or_undefined(&self.handle) }
    }

    /// Returns true if the value is a JavaScript object.
    ///
    /// Note: Unlike JavaScript's `typeof` operator which returns "object" for `null`,
    /// this method returns `false` for `null` values. Use `is_null_or_undefined()`
    /// to check for nullish values separately.
    pub fn is_object(&self) -> bool {
        unsafe { ffi::local_is_object(&self.handle) }
    }

    /// Returns true if the value is a native JavaScript error.
    pub fn is_native_error(&self) -> bool {
        unsafe { ffi::local_is_native_error(&self.handle) }
    }

    /// Returns true if the value is a JavaScript array.
    pub fn is_array(&self) -> bool {
        unsafe { ffi::local_is_array(&self.handle) }
    }

    /// Returns true if the value is a `Uint8Array`.
    pub fn is_uint8_array(&self) -> bool {
        unsafe { ffi::local_is_uint8_array(&self.handle) }
    }

    /// Returns true if the value is a `Uint16Array`.
    pub fn is_uint16_array(&self) -> bool {
        unsafe { ffi::local_is_uint16_array(&self.handle) }
    }

    /// Returns true if the value is a `Uint32Array`.
    pub fn is_uint32_array(&self) -> bool {
        unsafe { ffi::local_is_uint32_array(&self.handle) }
    }

    /// Returns true if the value is an `Int8Array`.
    pub fn is_int8_array(&self) -> bool {
        unsafe { ffi::local_is_int8_array(&self.handle) }
    }

    /// Returns true if the value is an `Int16Array`.
    pub fn is_int16_array(&self) -> bool {
        unsafe { ffi::local_is_int16_array(&self.handle) }
    }

    /// Returns true if the value is an `Int32Array`.
    pub fn is_int32_array(&self) -> bool {
        unsafe { ffi::local_is_int32_array(&self.handle) }
    }

    /// Returns true if the value is a `Float32Array`.
    pub fn is_float32_array(&self) -> bool {
        unsafe { ffi::local_is_float32_array(&self.handle) }
    }

    /// Returns true if the value is a `Float64Array`.
    pub fn is_float64_array(&self) -> bool {
        unsafe { ffi::local_is_float64_array(&self.handle) }
    }

    /// Returns true if the value is a `BigInt64Array`.
    pub fn is_bigint64_array(&self) -> bool {
        unsafe { ffi::local_is_bigint64_array(&self.handle) }
    }

    /// Returns true if the value is a `BigUint64Array`.
    pub fn is_biguint64_array(&self) -> bool {
        unsafe { ffi::local_is_biguint64_array(&self.handle) }
    }

    /// Returns true if the value is an `ArrayBuffer`.
    pub fn is_array_buffer(&self) -> bool {
        unsafe { ffi::local_is_array_buffer(&self.handle) }
    }

    /// Returns true if the value is an `ArrayBufferView`.
    pub fn is_array_buffer_view(&self) -> bool {
        unsafe { ffi::local_is_array_buffer_view(&self.handle) }
    }

    /// Returns the JavaScript type of the underlying value as a string.
    ///
    /// Uses V8's native `TypeOf` method which returns the same result as
    /// JavaScript's `typeof` operator: "undefined", "boolean", "number",
    /// "bigint", "string", "symbol", "function", or "object".
    ///
    /// Note: For `null`, this returns "object" (JavaScript's historical behavior).
    pub fn type_of(&self) -> String {
        unsafe { ffi::local_type_of(self.isolate.as_ffi(), &self.handle) }
    }

    /// Returns the isolate associated with this local handle.
    pub fn isolate(&self) -> IsolatePtr {
        self.isolate
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
        unsafe { ffi::local_to_global(lock.isolate().as_ffi(), self.into_ffi()).into() }
    }

    /// Casts this value to an Array.
    ///
    /// # Safety
    /// The caller must ensure this value is actually an array (check with `is_array()`).
    pub unsafe fn as_array(self) -> Local<'a, Array> {
        debug_assert!(self.is_array());
        unsafe { Local::from_ffi(self.isolate, self.into_ffi()) }
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

impl<'a> From<Local<'a, Function>> for Local<'a, Value> {
    fn from(value: Local<'a, Function>) -> Self {
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

// TODO: We need to figure out a smart way of avoiding duplication.
impl<'a> From<Local<'a, FunctionTemplate>> for Local<'a, Value> {
    fn from(value: Local<'a, FunctionTemplate>) -> Self {
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

impl<'a> From<Local<'a, Array>> for Local<'a, Value> {
    fn from(value: Local<'a, Array>) -> Self {
        debug_assert!(value.is_array());
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

impl Local<'_, Array> {
    /// Creates a new JavaScript array with the given length.
    pub fn new<'a>(lock: &mut crate::Lock, len: usize) -> Local<'a, Array> {
        let isolate = lock.isolate();
        unsafe { Local::from_ffi(isolate, ffi::local_new_array(isolate.as_ffi(), len)) }
    }

    /// Returns the length of the array.
    #[inline]
    pub fn len(&self) -> usize {
        unsafe { ffi::local_array_length(self.isolate.as_ffi(), &self.handle) as usize }
    }

    /// Returns true if the array is empty.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Sets an element at the given index.
    pub fn set(&mut self, index: usize, value: Local<'_, Value>) {
        unsafe {
            ffi::local_array_set(
                self.isolate.as_ffi(),
                &mut self.handle,
                index as u32,
                value.into_ffi(),
            );
        }
    }

    /// Iterates over array elements using V8's native `Array::Iterate()`.
    /// Returns Global handles because Local handles get reused during iteration.
    pub fn iterate(self) -> Vec<Global<Value>> {
        unsafe { ffi::local_array_iterate(self.isolate.as_ffi(), self.into_ffi()) }
            .into_iter()
            .map(|g| unsafe { Global::from_ffi(g) })
            .collect()
    }
}

impl<'a> From<Local<'a, Uint8Array>> for Local<'a, Value> {
    fn from(value: Local<'a, Uint8Array>) -> Self {
        debug_assert!(value.is_uint8_array());
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

impl<'a> From<Local<'a, Uint16Array>> for Local<'a, Value> {
    fn from(value: Local<'a, Uint16Array>) -> Self {
        debug_assert!(value.is_uint16_array());
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

impl<'a> From<Local<'a, Uint32Array>> for Local<'a, Value> {
    fn from(value: Local<'a, Uint32Array>) -> Self {
        debug_assert!(value.is_uint32_array());
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

impl<'a> From<Local<'a, Int8Array>> for Local<'a, Value> {
    fn from(value: Local<'a, Int8Array>) -> Self {
        debug_assert!(value.is_int8_array());
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

impl<'a> From<Local<'a, Int16Array>> for Local<'a, Value> {
    fn from(value: Local<'a, Int16Array>) -> Self {
        debug_assert!(value.is_int16_array());
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

impl<'a> From<Local<'a, Int32Array>> for Local<'a, Value> {
    fn from(value: Local<'a, Int32Array>) -> Self {
        debug_assert!(value.is_int32_array());
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

// TypedArray base type conversions
impl<'a> From<Local<'a, TypedArray>> for Local<'a, Value> {
    fn from(value: Local<'a, TypedArray>) -> Self {
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

impl<'a> From<Local<'a, Uint8Array>> for Local<'a, TypedArray> {
    fn from(value: Local<'a, Uint8Array>) -> Self {
        debug_assert!(value.is_uint8_array());
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

impl<'a> From<Local<'a, Uint16Array>> for Local<'a, TypedArray> {
    fn from(value: Local<'a, Uint16Array>) -> Self {
        debug_assert!(value.is_uint16_array());
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

impl<'a> From<Local<'a, Uint32Array>> for Local<'a, TypedArray> {
    fn from(value: Local<'a, Uint32Array>) -> Self {
        debug_assert!(value.is_uint32_array());
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

impl<'a> From<Local<'a, Int8Array>> for Local<'a, TypedArray> {
    fn from(value: Local<'a, Int8Array>) -> Self {
        debug_assert!(value.is_int8_array());
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

impl<'a> From<Local<'a, Int16Array>> for Local<'a, TypedArray> {
    fn from(value: Local<'a, Int16Array>) -> Self {
        debug_assert!(value.is_int16_array());
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

impl<'a> From<Local<'a, Int32Array>> for Local<'a, TypedArray> {
    fn from(value: Local<'a, Int32Array>) -> Self {
        debug_assert!(value.is_int32_array());
        unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
    }
}

// `TypedArray`-specific implementations
impl Local<'_, TypedArray> {
    /// Returns the number of elements in this `TypedArray`.
    pub fn len(&self) -> usize {
        unsafe { ffi::local_typed_array_length(self.isolate.as_ffi(), &self.handle) }
    }

    /// Returns true if the `TypedArray` is empty.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

// =============================================================================
// `TypedArray` Iterator Types
// =============================================================================

/// Iterator over `TypedArray` elements by reference.
///
/// Created by calling `iter()` on a `Local<'a, TypedArray>`.
/// Does not consume the array, allowing multiple iterations.
pub struct TypedArrayIter<'a, 'b, T, E> {
    array: &'b Local<'a, T>,
    index: usize,
    len: usize,
    _marker: PhantomData<E>,
}

/// Owning iterator over `TypedArray` elements.
///
/// Created by calling `into_iter()` on a `Local<'a, TypedArray>`.
/// Consumes the array handle.
pub struct TypedArrayIntoIter<'a, T, E> {
    array: Local<'a, T>,
    index: usize,
    len: usize,
    _marker: PhantomData<E>,
}

// =============================================================================
// `TypedArray` Implementation Macro
// =============================================================================

/// Implements methods and traits for a specific `TypedArray` type.
///
/// For each `TypedArray` marker type (e.g., `Uint8Array`), this macro generates:
/// - `len()`, `is_empty()`, `get(index)` methods
/// - `iter()` method for borrowing iteration
/// - `IntoIterator` for owned and borrowed iteration
/// - `Iterator` implementations for both iterator types
macro_rules! impl_typed_array {
    ($marker:ident, $elem:ty, $get_fn:ident) => {
        impl<'a> Local<'a, $marker> {
            /// Returns the number of elements in this `TypedArray`.
            #[inline]
            pub fn len(&self) -> usize {
                unsafe { ffi::local_typed_array_length(self.isolate.as_ffi(), &self.handle) }
            }

            /// Returns `true` if the `TypedArray` contains no elements.
            #[inline]
            pub fn is_empty(&self) -> bool {
                self.len() == 0
            }

            /// Returns the element at `index`.
            ///
            /// # Panics
            ///
            /// Panics if `index >= self.len()`.
            #[inline]
            pub fn get(&self, index: usize) -> $elem {
                debug_assert!(index < self.len(), "index out of bounds");
                unsafe { ffi::$get_fn(self.isolate.as_ffi(), &self.handle, index) }
            }

            /// Returns an iterator over the elements.
            ///
            /// The iterator yields elements by value (copied from V8 memory).
            #[inline]
            pub fn iter(&self) -> TypedArrayIter<'a, '_, $marker, $elem> {
                TypedArrayIter {
                    array: self,
                    index: 0,
                    len: self.len(),
                    _marker: PhantomData,
                }
            }
        }

        // Owned iteration: `for x in array`
        impl<'a> IntoIterator for Local<'a, $marker> {
            type Item = $elem;
            type IntoIter = TypedArrayIntoIter<'a, $marker, $elem>;

            #[inline]
            fn into_iter(self) -> Self::IntoIter {
                let len = self.len();
                TypedArrayIntoIter {
                    array: self,
                    index: 0,
                    len,
                    _marker: PhantomData,
                }
            }
        }

        // Borrowed iteration: `for x in &array`
        impl<'a, 'b> IntoIterator for &'b Local<'a, $marker> {
            type Item = $elem;
            type IntoIter = TypedArrayIter<'a, 'b, $marker, $elem>;

            #[inline]
            fn into_iter(self) -> Self::IntoIter {
                self.iter()
            }
        }

        impl<'a, 'b> Iterator for TypedArrayIter<'a, 'b, $marker, $elem> {
            type Item = $elem;

            #[inline]
            fn next(&mut self) -> Option<Self::Item> {
                if self.index < self.len {
                    let value = unsafe {
                        ffi::$get_fn(self.array.isolate.as_ffi(), &self.array.handle, self.index)
                    };
                    self.index += 1;
                    Some(value)
                } else {
                    None
                }
            }

            #[inline]
            fn size_hint(&self) -> (usize, Option<usize>) {
                let remaining = self.len - self.index;
                (remaining, Some(remaining))
            }
        }

        impl<'a, 'b> ExactSizeIterator for TypedArrayIter<'a, 'b, $marker, $elem> {}

        impl<'a, 'b> DoubleEndedIterator for TypedArrayIter<'a, 'b, $marker, $elem> {
            #[inline]
            fn next_back(&mut self) -> Option<Self::Item> {
                if self.index < self.len {
                    self.len -= 1;
                    let value = unsafe {
                        ffi::$get_fn(self.array.isolate.as_ffi(), &self.array.handle, self.len)
                    };
                    Some(value)
                } else {
                    None
                }
            }
        }

        impl<'a, 'b> std::iter::FusedIterator for TypedArrayIter<'a, 'b, $marker, $elem> {}

        impl<'a> Iterator for TypedArrayIntoIter<'a, $marker, $elem> {
            type Item = $elem;

            #[inline]
            fn next(&mut self) -> Option<Self::Item> {
                if self.index < self.len {
                    let value = unsafe {
                        ffi::$get_fn(self.array.isolate.as_ffi(), &self.array.handle, self.index)
                    };
                    self.index += 1;
                    Some(value)
                } else {
                    None
                }
            }

            #[inline]
            fn size_hint(&self) -> (usize, Option<usize>) {
                let remaining = self.len - self.index;
                (remaining, Some(remaining))
            }
        }

        impl<'a> ExactSizeIterator for TypedArrayIntoIter<'a, $marker, $elem> {}

        impl<'a> DoubleEndedIterator for TypedArrayIntoIter<'a, $marker, $elem> {
            #[inline]
            fn next_back(&mut self) -> Option<Self::Item> {
                if self.index < self.len {
                    self.len -= 1;
                    let value = unsafe {
                        ffi::$get_fn(self.array.isolate.as_ffi(), &self.array.handle, self.len)
                    };
                    Some(value)
                } else {
                    None
                }
            }
        }

        impl<'a> std::iter::FusedIterator for TypedArrayIntoIter<'a, $marker, $elem> {}
    };
}

impl_typed_array!(Uint8Array, u8, local_uint8_array_get);
impl_typed_array!(Uint16Array, u16, local_uint16_array_get);
impl_typed_array!(Uint32Array, u32, local_uint32_array_get);
impl_typed_array!(Int8Array, i8, local_int8_array_get);
impl_typed_array!(Int16Array, i16, local_int16_array_get);
impl_typed_array!(Int32Array, i32, local_int32_array_get);
impl_typed_array!(Float32Array, f32, local_float32_array_get);
impl_typed_array!(Float64Array, f64, local_float64_array_get);
impl_typed_array!(BigInt64Array, i64, local_bigint64_array_get);
impl_typed_array!(BigUint64Array, u64, local_biguint64_array_get);

// Object-specific implementations
impl<'a> Local<'a, Object> {
    pub fn set(&mut self, lock: &mut Lock, key: &str, value: Local<'a, Value>) {
        unsafe {
            ffi::local_object_set_property(
                lock.isolate().as_ffi(),
                &mut self.handle,
                key,
                value.into_ffi(),
            );
        }
    }

    pub fn has(&self, lock: &mut Lock, key: &str) -> bool {
        unsafe { ffi::local_object_has_property(lock.isolate().as_ffi(), &self.handle, key) }
    }

    pub fn get(&self, lock: &mut Lock, key: &str) -> Option<Local<'a, Value>> {
        if !self.has(lock, key) {
            return None;
        }

        unsafe {
            let maybe_local =
                ffi::local_object_get_property(lock.isolate().as_ffi(), &self.handle, key);
            let opt_local: Option<ffi::Local> = maybe_local.into();
            opt_local.map(|local| Local::from_ffi(lock.isolate(), local))
        }
    }
}

impl<'a> From<Local<'a, Value>> for Local<'a, Object> {
    fn from(value: Local<'a, Value>) -> Self {
        debug_assert!(value.is_object());
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

    /// Returns a mutable reference to the underlying FFI handle.
    ///
    /// # Safety
    /// The caller must ensure the returned reference is not used after this `Global` is dropped.
    pub unsafe fn as_ffi_mut(&mut self) -> *mut ffi::Global {
        &raw mut self.handle
    }

    pub fn as_local<'a>(&self, lock: &mut Lock) -> Local<'a, T> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::global_to_local(lock.isolate().as_ffi(), &self.handle),
            )
        }
    }

    /// Resets this global handle, releasing the persistent reference.
    ///
    /// # Safety
    /// The caller must ensure the global handle is valid.
    pub unsafe fn reset(&mut self) {
        unsafe {
            ffi::global_reset(&raw mut self.handle);
        }
    }
}

impl<T> From<Local<'_, T>> for Global<T> {
    fn from(local: Local<'_, T>) -> Self {
        Self {
            handle: unsafe { ffi::local_to_global(local.isolate.as_ffi(), local.into_ffi()) },
            _marker: PhantomData,
        }
    }
}

impl Global<FunctionTemplate> {
    /// Returns the constructor function for this function template.
    ///
    /// This is the V8 `Function` object that can be called as a constructor or
    /// used to access static methods, analogous to a JavaScript class reference
    /// (e.g., `URL`, `TextEncoder`).
    pub fn as_local_function<'a>(&self, lock: &mut Lock) -> Local<'a, Function> {
        // SAFETY: `lock` guarantees the isolate is locked and a HandleScope is active.
        // `self.handle` is a valid `Global<FunctionTemplate>` created by `create_resource_template`.
        // The returned `Local` is tied to the current HandleScope via the `'a` lifetime.
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::function_template_get_function(lock.isolate().as_ffi(), &self.handle),
            )
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
        unsafe { self.reset() };
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

macro_rules! impl_to_local_value_integer {
    ($($type:ty),*) => {
        $(
            impl ToLocalValue for $type {
                fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
                    unsafe {
                        Local::from_ffi(
                            lock.isolate(),
                            ffi::local_new_number(lock.isolate().as_ffi(), f64::from(*self)),
                        )
                    }
                }
            }
        )*
    };
}

impl_to_local_value_integer!(u8, u16, u32, i8, i16, i32);

impl ToLocalValue for String {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        self.as_str().to_local(lock)
    }
}

impl ToLocalValue for &str {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_new_string(lock.isolate().as_ffi(), self),
            )
        }
    }
}

impl ToLocalValue for bool {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_new_boolean(lock.isolate().as_ffi(), *self),
            )
        }
    }
}

impl ToLocalValue for Number {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_new_number(lock.isolate().as_ffi(), self.value()),
            )
        }
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
    pub fn isolate(&self) -> IsolatePtr {
        unsafe { IsolatePtr::from_ffi(ffi::fci_get_isolate(self.0)) }
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
        debug_assert!(index <= self.len(), "index out of bounds");
        unsafe { Local::from_ffi(self.isolate(), ffi::fci_get_arg(self.0, index)) }
    }

    pub fn set_return_value(&self, value: Local<Value>) {
        unsafe {
            ffi::fci_set_return_value(self.0, value.into_ffi());
        }
    }
}

/// A fat pointer to a `dyn GarbageCollected` trait object, decomposed into its
/// data pointer and vtable pointer halves.
///
/// Rust trait object pointers (`*mut dyn Trait`) are fat pointers with the
/// layout `[data_ptr, vtable_ptr]`. We decompose them so the two halves can be
/// stored in the C++ `Wrappable::data[0..1]` slots (`uintptr_t`).
///
/// # Safety of the transmute
///
/// Rust guarantees that `*mut dyn Trait` is two pointers wide (the "fat
/// pointer" representation). We transmute to `[*mut (); 2]` to split and
/// rejoin. While the *exact* field order (`[data, vtable]`) is not
/// stabilised in a language RFC, it has been the layout since Rust 1.0 and
/// is relied upon by miri, the compiler test suite, and the unstable
/// `ptr_metadata` API. A layout change would break the ecosystem; we add a
/// compile-time size assert as a safety net.
///
/// TODO(rust-lang/rust#81513): Replace the transmute with `std::ptr::metadata`
/// / `std::ptr::from_raw_parts_mut` once the `ptr_metadata` feature is
/// stabilised. That will make the `[data, vtable]` ordering an explicit API
/// contract rather than an assumed layout.
// Fat pointer must be exactly two pointers wide.
const _: () = assert!(
    size_of::<*mut dyn GarbageCollected>() == 2 * size_of::<usize>(),
    "trait object pointer must be two pointers wide",
);

pub(crate) struct TraitObjectPtr {
    pub(crate) data: NonNull<()>,
    /// The vtable pointer, stored as `usize` for C++ interop.
    pub(crate) vtable: usize,
}

impl TraitObjectPtr {
    /// Creates a `TraitObjectPtr` from a raw `*mut dyn GarbageCollected` fat pointer.
    ///
    /// Typically used with an `Rc::into_raw` pointer coerced to
    /// `*mut dyn GarbageCollected`.
    pub(crate) fn from_raw(ptr: *mut dyn GarbageCollected) -> Self {
        // SAFETY: a fat pointer is layout-equivalent to [*mut (); 2].
        let [data, vtable]: [*mut (); 2] = unsafe { std::mem::transmute(ptr) };
        Self {
            data: NonNull::new(data).expect("Rc::into_raw returned null"),
            vtable: vtable as usize,
        }
    }

    /// Reads the fat pointer from a Wrappable's `data[0..1]`.
    ///
    /// Returns `None` if `data[0]` is null (resource already dropped).
    ///
    /// # Safety
    /// The wrappable must be valid. If `data[0]` is non-null, the fat pointer
    /// must still refer to a live `Rc`-backed `dyn GarbageCollected` allocation.
    unsafe fn from_wrappable(wrappable: &ffi::Wrappable) -> Option<Self> {
        // SAFETY: caller guarantees wrappable is valid. If data[0] is non-null,
        // the two slots form a valid fat pointer to a live dyn GarbageCollected.
        unsafe {
            let slots = ffi::wrappable_data(wrappable);
            let data_val = *slots;
            if data_val == 0 {
                return None;
            }
            let vtable_val = *slots.add(1);
            Some(Self {
                data: NonNull::new_unchecked(data_val as *mut ()),
                vtable: vtable_val,
            })
        }
    }

    /// Reconstructs a shared reference to the `dyn GarbageCollected` object.
    ///
    /// # Safety
    /// The original object must still be alive for lifetime `'a`.
    #[expect(clippy::needless_lifetimes)]
    unsafe fn as_gc_ref<'a>(&'a self) -> &'a dyn GarbageCollected {
        // SAFETY: transmuting [data, vtable] back into a fat pointer. The
        // original object must still be alive for lifetime 'a.
        unsafe {
            let fat_ptr: *const dyn GarbageCollected =
                std::mem::transmute([self.data.as_ptr(), self.vtable as *mut ()]);
            &*fat_ptr
        }
    }

    /// Reconstructs the `Rc<dyn GarbageCollected>` and drops it, decrementing
    /// the `Rc` refcount and potentially freeing the resource.
    ///
    /// # Safety
    /// The data pointer must have originated from `Rc::into_raw`.
    /// Must only be called once per allocation.
    unsafe fn drop_rc(self) {
        // SAFETY: transmuting [data, vtable] back into a fat pointer. The data
        // pointer originated from Rc::into_raw, so Rc::from_raw is valid.
        unsafe {
            let fat_ptr: *const dyn GarbageCollected =
                std::mem::transmute([self.data.as_ptr().cast_const(), self.vtable as *const ()]);
            drop(Rc::from_raw(fat_ptr));
        }
    }

    /// Zeroes the `data[0..1]` slots in a Wrappable.
    ///
    /// Called before `drop_rc` so that any re-entrant `wrappable_invoke_trace`
    /// calls during destruction see null and no-op.
    ///
    /// # Safety
    /// Must only be called while holding exclusive access (e.g. via `Pin<&mut Wrappable>`).
    unsafe fn clear_wrappable_data(wrappable: &ffi::Wrappable) {
        // SAFETY: caller holds exclusive access (via Pin<&mut Wrappable>).
        unsafe {
            let data = ffi::wrappable_data(wrappable).cast_mut();
            data.write(0);
            data.add(1).write(0);
        }
    }
}

/// `TraitObjectPtr` → `WrappableRc`: allocates a new Wrappable on the KJ heap,
/// transferring ownership of the `Rc`-backed `dyn GarbageCollected` fat pointer.
///
/// # Safety
/// The data pointer must have come from `Rc::into_raw` and must not be used to
/// reconstruct the Rc after this call (the Wrappable now owns it).
impl From<TraitObjectPtr> for WrappableRc {
    fn from(ptr: TraitObjectPtr) -> Self {
        Self {
            handle: unsafe { ffi::wrappable_new(ptr.data.as_ptr() as usize, ptr.vtable) },
        }
    }
}

#[expect(
    clippy::needless_pass_by_value,
    reason = "signature is mandated by CXX bridge declaration"
)]
unsafe fn wrappable_invoke_drop(wrappable: Pin<&mut ffi::Wrappable>) {
    // SAFETY: wrappable is valid and exclusively accessed (Pin<&mut>).
    // Clear data slots before drop to prevent re-entrant trace during destruction,
    // then drop the Rc<dyn GarbageCollected> to release the resource.
    unsafe {
        let Some(trait_ptr) = TraitObjectPtr::from_wrappable(wrappable.as_ref().get_ref()) else {
            return;
        };
        TraitObjectPtr::clear_wrappable_data(wrappable.as_ref().get_ref());
        trait_ptr.drop_rc();
    }
}

unsafe fn wrappable_invoke_trace(wrappable: &ffi::Wrappable, visitor: *mut ffi::GcVisitor) {
    // SAFETY: wrappable is valid. visitor is a valid C++ GcVisitor pointer.
    unsafe {
        let Some(trait_ptr) = TraitObjectPtr::from_wrappable(wrappable) else {
            return;
        };
        let mut gc_visitor = GcVisitor::from_ffi(visitor);
        trait_ptr.as_gc_ref().trace(&mut gc_visitor);
    }
}

unsafe fn wrappable_invoke_get_name(wrappable: &ffi::Wrappable) -> *const c_char {
    // SAFETY: wrappable is valid.
    unsafe {
        let Some(trait_ptr) = TraitObjectPtr::from_wrappable(wrappable) else {
            return std::ptr::null();
        };
        trait_ptr
            .as_gc_ref()
            .get_name()
            .map_or(std::ptr::null(), std::ffi::CStr::as_ptr)
    }
}

/// Visitor for garbage collection tracing.
///
/// `GcVisitor` wraps a C++ `jsg::GcVisitor` pointer. All GC visitation logic
/// (strong/traced switching, parent tracking) is handled by the C++ side via
/// `Wrappable::visitRef()`.
#[derive(Debug)]
pub struct GcVisitor {
    pub(crate) handle: ffi::GcVisitor,
}

impl GcVisitor {
    /// Creates a `GcVisitor` from a raw FFI pointer.
    ///
    /// # Safety
    ///
    /// `visitor` must be a valid, non-null pointer to a live `ffi::GcVisitor`.
    pub(crate) unsafe fn from_ffi(visitor: *mut ffi::GcVisitor) -> Self {
        Self {
            handle: ffi::GcVisitor {
                ptr: unsafe { (*visitor).ptr },
            },
        }
    }

    /// Visits a `Ref` during GC tracing.
    ///
    /// Delegates to the C++ `Wrappable::visitRef()` which handles all the
    /// strong/traced switching logic and transitive tracing.
    pub fn visit_ref<R: crate::Resource>(&mut self, r: &crate::Ref<R>) {
        r.visit(self);
    }
}

/// A safe wrapper around a V8 isolate pointer.
///
/// `IsolatePtr` provides a type-safe abstraction over raw `v8::Isolate*` pointers,
/// ensuring that the pointer is always non-null. This type is `Copy` and can be
/// freely passed around without worrying about ownership.
///
/// # Thread Safety
///
/// V8 isolates are single-threaded. While `IsolatePtr` itself is `Send` and `Sync`
/// (as it's just a pointer wrapper), V8 operations must only be performed on the
/// thread that owns the isolate lock. Use `is_locked()` to verify the current
/// thread holds the lock before performing V8 operations.
///
/// # Example
///
/// ```ignore
/// // Create from raw pointer (unsafe)
/// let isolate = unsafe { v8::IsolatePtr::from_ffi(raw_ptr) };
///
/// // Check if locked before V8 operations
/// assert!(unsafe { isolate.is_locked() });
///
/// // Get raw pointer for FFI calls
/// let ptr = isolate.as_ffi();
/// ```
#[derive(Clone, Copy, Debug)]
pub struct IsolatePtr {
    handle: NonNull<ffi::Isolate>,
}

impl IsolatePtr {
    /// Creates an `IsolatePtr` from a raw pointer.
    ///
    /// # Safety
    /// The pointer must be non-null and point to a valid V8 isolate.
    pub unsafe fn from_ffi(handle: *mut ffi::Isolate) -> Self {
        debug_assert!(unsafe { ffi::isolate_is_locked(handle) });
        Self {
            handle: unsafe { NonNull::new_unchecked(handle) },
        }
    }

    /// Creates an `IsolatePtr` from a `NonNull` pointer.
    pub fn from_non_null(handle: NonNull<ffi::Isolate>) -> Self {
        debug_assert!(unsafe { ffi::isolate_is_locked(handle.as_ptr()) });
        Self { handle }
    }

    /// Returns whether this isolate is currently locked by the current thread.
    ///
    /// # Safety
    ///
    /// The caller must ensure the isolate is still valid and not deallocated.
    pub unsafe fn is_locked(&self) -> bool {
        unsafe { ffi::isolate_is_locked(self.handle.as_ptr()) }
    }

    /// Returns the raw pointer to the V8 isolate.
    pub fn as_ffi(&self) -> *mut ffi::Isolate {
        self.handle.as_ptr()
    }

    /// Returns the `NonNull` pointer to the V8 isolate.
    pub fn as_non_null(&self) -> NonNull<ffi::Isolate> {
        self.handle
    }
}

// =============================================================================
// Wrappable — owned, reference-counted handle
// =============================================================================

/// Owned, reference-counted handle to a C++ `Wrappable` on the KJ heap.
///
/// Encapsulates `KjRc<ffi::Wrappable>` so that modules outside `v8` never
/// reference the CXX-generated `ffi::Wrappable` type directly.
///
/// `Clone` / `Drop` only affect the `KjRc` refcount (`kj::Rc` reference counting).
/// GC strong-ref tracking (`addStrongRef` / `removeStrongRef`) is handled by
/// `Ref<R>`, not here.
#[derive(Clone)]
pub struct WrappableRc {
    handle: kj_rs::KjRc<ffi::Wrappable>,
}

impl WrappableRc {
    /// Unwraps a JavaScript value to get an owned Wrappable handle.
    ///
    /// Returns `None` if the value is not a Rust-tagged Wrappable
    /// (e.g. a C++ JSG object, a plain JS object, or a primitive).
    ///
    /// The C++ `unwrap_resource` returns a `KjRc<Wrappable>` whose inner
    /// pointer is null when the value doesn't contain a Rust Wrappable.
    /// We check `get().is_null()` to distinguish that case.
    pub(crate) fn from_js(isolate: IsolatePtr, value: Local<Value>) -> Option<Self> {
        let handle = unsafe { ffi::unwrap_resource(isolate.as_ffi(), value.into_ffi()) };
        if handle.get().is_null() {
            return None;
        }
        Some(Self { handle })
    }

    /// Wraps this Wrappable as a JavaScript object using the given constructor template.
    pub(crate) fn to_js<'a>(
        &self,
        isolate: IsolatePtr,
        constructor: &Global<FunctionTemplate>,
    ) -> Local<'a, Value> {
        unsafe {
            Local::from_ffi(
                isolate,
                ffi::wrap_resource(
                    isolate.as_ffi(),
                    self.handle.clone(),
                    constructor.as_ffi_ref(),
                ),
            )
        }
    }

    /// Creates an owning `WrappableRc` from a raw `*const ffi::Wrappable` pointer.
    ///
    /// Increments the `kj::Rc` refcount via `addRefToThis()`.
    ///
    /// # Safety
    /// The pointed-to Wrappable must still be alive.
    pub(crate) unsafe fn from_raw_wrappable(ptr: *const ffi::Wrappable) -> Self {
        // SAFETY: ptr is valid and alive (caller verified via Weak::upgrade).
        // The const-to-mut cast is sound for the same reason as as_pin_mut().
        // wrappable_to_rc calls addRefToThis() which uses interior mutability.
        unsafe {
            let wrappable = Pin::new_unchecked(&mut *ptr.cast_mut());
            Self {
                handle: ffi::wrappable_to_rc(wrappable),
            }
        }
    }

    /// Returns a `Pin<&mut ffi::Wrappable>` for C++ FFI calls.
    ///
    /// Takes `&self` because the mutation target is the C++ `Wrappable` on the
    /// KJ heap (behind the raw pointer in `KjRc`), not the `WrappableRc` wrapper
    /// itself. `KjRc::get()` returns `*const T`; the const-to-mut cast is
    /// required because CXX maps non-const C++ references (`T&`) to
    /// `Pin<&mut T>`. `KjRc::get_mut()` cannot be used because it returns
    /// `None` when the refcount > 1 (the common case).
    ///
    /// # Safety
    ///
    /// The returned `Pin<&mut Wrappable>` must be used transiently — passed
    /// directly into a C++ FFI call and never stored. This prevents aliased
    /// `&mut` references from coexisting. The invariant is enforced by:
    ///
    /// 1. **`pub(crate)` visibility** — only code in this crate can call this.
    /// 2. **Single-threaded V8 isolate** — all callers run on the isolate's
    ///    thread, so no concurrent access is possible.
    /// 3. **Non-reentrant FFI** — the C++ methods called through this pin
    ///    (`addStrongRef`, `removeStrongRef`, `visitRef`) do not call back
    ///    into Rust in a way that would create a second `Pin<&mut Wrappable>`
    ///    for the same object.
    #[expect(
        clippy::mut_from_ref,
        reason = "Pin<&mut> comes from a raw pointer, not from &self"
    )]
    unsafe fn as_pin_mut(&self) -> Pin<&mut ffi::Wrappable> {
        unsafe { Pin::new_unchecked(&mut *self.handle.get().cast_mut()) }
    }

    /// Visits this Wrappable during GC tracing.
    ///
    /// Takes `&self` because this is called from `Ref::visit(&self)` which is
    /// called from `GarbageCollected::trace(&self)`. The mutation target is
    /// the C++ `Wrappable` on the KJ heap, not the `WrappableRc` wrapper.
    pub(crate) fn visit_ref(&self, parent: *mut usize, strong: *mut bool, visitor: &mut GcVisitor) {
        unsafe {
            ffi::wrappable_visit_ref(
                self.as_pin_mut(),
                parent,
                strong,
                std::ptr::from_mut(&mut visitor.handle),
            );
        }
    }

    /// Returns a `NonNull` pointer to the underlying `ffi::Wrappable`.
    ///
    /// The `KjRc` always holds a valid, non-null pointer to the Wrappable on
    /// the KJ heap.
    pub(crate) fn as_ptr(&self) -> NonNull<ffi::Wrappable> {
        // SAFETY: KjRc always holds a valid, non-null pointer.
        unsafe { NonNull::new_unchecked(self.handle.get().cast_mut()) }
    }

    /// Increments the strong reference count on the underlying Wrappable.
    ///
    /// Called when a new `Ref<R>` is created (clone, unwrap) to inform the GC
    /// that this Wrappable has an additional strong reference from Rust.
    pub(crate) fn add_strong_ref(&mut self) {
        unsafe { ffi::wrappable_add_strong_ref(self.as_pin_mut()) };
    }

    /// Decrements the strong reference count and potentially defers destruction.
    ///
    /// Called when a `Ref<R>` is dropped. Calls `removeStrongRef` +
    /// `maybeDeferDestruction` on the C++ side, then the `KjRc` drop (when
    /// the last `WrappableRc` clone goes away) handles the final refcount decrement.
    pub(crate) fn remove_strong_ref(&mut self) {
        unsafe { ffi::wrappable_remove_strong_ref(self.as_pin_mut()) };
    }

    /// Resolves the resource pointer from the Wrappable's `data[0]` field.
    ///
    /// The data pointer is the `Rc::into_raw` pointer from `alloc`, which
    /// points directly to `R` inside the `Rc` allocation.
    pub(crate) fn resolve_resource<R: Resource>(&self) -> NonNull<R> {
        // SAFETY: KjRc always holds a valid pointer. The wrappable was created
        // by wrappable_new and data[0] is the Rc::into_raw pointer to R.
        let trait_ptr = unsafe {
            let wrappable = &*self.handle.get();
            TraitObjectPtr::from_wrappable(wrappable)
        }
        .expect("wrappable data already cleared");
        // data[0] is Rc::into_raw(*const R) — points directly to R.
        trait_ptr.data.cast::<R>()
    }
}
