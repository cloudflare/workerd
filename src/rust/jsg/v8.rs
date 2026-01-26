use core::ffi::c_void;
use std::fmt::Display;
use std::marker::PhantomData;
use std::ptr::NonNull;

use crate::FromJS;
use crate::Lock;
use crate::Number;

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

    /// Module visibility level, corresponds to `workerd::jsg::ModuleType` from modules.capnp.
    /// Values are automatically assigned by `cxx` because of extern declaration below.
    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    #[repr(u16)]
    enum ModuleType {
        BUNDLE,
        BUILTIN,
        INTERNAL,
    }
    unsafe extern "C++" {
        type ModuleType;
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

    pub fn as_local<'a>(&self, lock: &mut Lock) -> Local<'a, T> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::global_to_local(lock.isolate().as_ffi(), &self.handle),
            )
        }
    }

    /// Makes this global handle weak, allowing V8 to garbage collect the object
    /// and invoke the callback when the object is being collected.
    ///
    /// # Safety
    /// The caller must ensure:
    /// - `isolate` is a valid V8 isolate wrapper
    /// - `data` encodes a value that remains valid until the callback is invoked
    /// - `callback` can safely handle the provided data value
    pub unsafe fn make_weak(
        &mut self,
        isolate: IsolatePtr,
        data: *mut c_void,
        callback: fn(*mut ffi::Isolate, usize) -> (),
    ) {
        unsafe {
            ffi::global_make_weak(
                isolate.as_ffi(),
                &raw mut self.handle,
                data as usize,
                callback,
            );
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
                ffi::local_new_number(lock.isolate().as_ffi(), f64::from(*self)),
            )
        }
    }
}

impl ToLocalValue for u32 {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_new_number(lock.isolate().as_ffi(), f64::from(*self)),
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

/// A safe wrapper around a V8 isolate pointer.
///
/// `Isolate` provides a type-safe abstraction over raw `v8::Isolate*` pointers,
/// ensuring that the pointer is always non-null. This type is `Copy` and can be
/// freely passed around without worrying about ownership.
///
/// # Thread Safety
///
/// V8 isolates are single-threaded. While `Isolate` itself is `Send` and `Sync`
/// (as it's just a pointer wrapper), V8 operations must only be performed on the
/// thread that owns the isolate lock. Use `is_locked()` to verify the current
/// thread holds the lock before performing V8 operations.
///
/// # Example
///
/// ```ignore
/// // Create from raw pointer (unsafe)
/// let isolate = unsafe { v8::Isolate::from_ffi(raw_ptr) };
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
    /// Creates an `Isolate` from a raw pointer.
    ///
    /// # Safety
    /// The pointer must be non-null and point to a valid V8 isolate.
    pub unsafe fn from_ffi(handle: *mut ffi::Isolate) -> Self {
        debug_assert!(unsafe { ffi::isolate_is_locked(handle) });
        Self {
            handle: unsafe { NonNull::new_unchecked(handle) },
        }
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
}
