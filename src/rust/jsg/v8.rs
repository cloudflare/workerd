use std::marker::PhantomData;

use crate::Lock;

#[expect(clippy::missing_safety_doc)]
#[cxx::bridge(namespace = "workerd::rust::jsg")]
pub mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type Isolate;
        type FunctionCallbackInfo;

        pub unsafe fn get_isolate(args: *mut FunctionCallbackInfo) -> *mut Isolate;
        pub unsafe fn get_this(args: *mut FunctionCallbackInfo) -> usize /* LocalValue> */;
        pub unsafe fn get_length(args: *mut FunctionCallbackInfo) -> usize;
        pub unsafe fn get_arg(args: *mut FunctionCallbackInfo, index: usize) -> usize /* LocalValue> */;
        pub unsafe fn unwrap_string(
            isolate: *mut Isolate,
            value: usize, /* LocalValue> */
        ) -> String;
        pub unsafe fn global_function_template_as_local(
            isolate: *mut Isolate,
            value: usize, /* GlobalFunctionTemplate */
        ) -> usize /* LocalFunctionTemplate */;

        pub unsafe fn new_local_number(isolate: *mut Isolate, value: f64) -> usize /* Local<Value> */;

        pub unsafe fn new_local_string(isolate: *mut Isolate, value: &str) -> usize /* Local<Value> */;

        pub unsafe fn new_local_object(isolate: *mut Isolate) -> usize /* Local<Object> */;
        pub unsafe fn set_local_object_property(
            isolate: *mut Isolate,
            object: usize, /* Local<Object> */
            key: &str,
            value: usize, /* Local<Value> */
        );

        pub unsafe fn set_return_value(
            args: *mut FunctionCallbackInfo,
            value: usize, /* Local<Value> */
        );

        pub unsafe fn local_to_global_value(
            isolate: *mut Isolate,
            value: usize, /* Local<Value> */
        ) -> usize /* Global<Value> */;
        pub unsafe fn global_to_local_value(
            isolate: *mut Isolate,
            value: usize, /* Global<Value> */
        ) -> usize /* Local<Value> */;

        pub unsafe fn clone_local_value(
            isolate: *mut Isolate,
            value: usize, /* Local<Value> */
        ) -> usize /* Local<Value> */;
        pub unsafe fn clone_global_value(
            isolate: *mut Isolate,
            value: usize, /* Global<Value> */
        ) -> usize /* Global<Value> */;

        pub unsafe fn eq_local_value(
            lhs: usize, /* Local<Value> */
            rhs: usize, /* Local<Value> */
        ) -> bool;
    }
}

#[derive(Debug)]
pub(crate) struct Handle {
    ptr: usize,
    isolate: *mut ffi::Isolate,
}

impl Handle {
    /// # Safety
    /// The caller must ensure that `isolate` is a valid pointer.
    pub unsafe fn new(ptr: usize, isolate: *mut ffi::Isolate) -> Self {
        Self { ptr, isolate }
    }

    #[expect(clippy::wrong_self_convention)]
    pub fn to_ffi(self) -> usize {
        self.ptr
    }

    /// # Safety
    /// Returns the underlying pointer value.
    pub unsafe fn as_ref(&self) -> usize {
        self.ptr
    }

    pub fn isolate(&self) -> *mut ffi::Isolate {
        self.isolate
    }
}

// Marker types for Local<T>
pub struct Value;
pub struct Object;
pub struct FunctionTemplate;

// Generic Local<'a, T> handle with lifetime
#[derive(Debug)]
pub struct Local<'a, T> {
    pub(crate) handle: Handle,
    _marker: PhantomData<(&'a (), T)>,
}

// Common implementations for all Local<'a, T>
#[expect(clippy::elidable_lifetime_names)]
impl<'a, T> Local<'a, T> {
    /// # Safety
    /// The caller must ensure that `isolate` and `value` are valid.
    pub unsafe fn from_ffi(isolate: *mut ffi::Isolate, value: usize) -> Self {
        Local {
            handle: unsafe { Handle::new(value, isolate) },
            _marker: PhantomData,
        }
    }

    /// # Safety
    /// Converts the Local handle to its FFI representation.
    pub unsafe fn to_ffi(self) -> usize {
        self.handle.to_ffi()
    }
}

impl<T> Clone for Local<'_, T> {
    fn clone(&self) -> Self {
        unsafe {
            Self::from_ffi(
                self.handle.isolate(),
                ffi::clone_local_value(self.handle.isolate(), self.handle.as_ref()),
            )
        }
    }
}

// Value-specific implementations
impl<'a> Local<'a, Value> {
    pub fn to_global(self, lock: &'a mut Lock) -> Global<Value> {
        unsafe {
            Global::from_ffi(
                lock.get_isolate(),
                ffi::local_to_global_value(lock.get_isolate(), self.handle.to_ffi()),
            )
        }
    }
}

impl PartialEq for Local<'_, Value> {
    fn eq(&self, other: &Self) -> bool {
        unsafe { ffi::eq_local_value(self.clone().handle.to_ffi(), other.clone().handle.to_ffi()) }
    }
}

impl<'a> From<Local<'a, Object>> for Local<'a, Value> {
    fn from(value: Local<'a, Object>) -> Self {
        Local {
            handle: value.handle,
            _marker: PhantomData,
        }
    }
}

// Object-specific implementations
impl<'a> Local<'a, Object> {
    /// # Safety
    /// The caller must ensure that the object and value are valid and the lock is properly held.
    pub unsafe fn set(&mut self, lock: &mut Lock, key: &str, value: Local<'a, Value>) {
        unsafe {
            ffi::set_local_object_property(
                lock.get_isolate(),
                self.clone().handle.to_ffi(),
                key,
                value.handle.to_ffi(),
            );
        }
    }
}

// Generic Global<T> handle without lifetime
pub struct Global<T> {
    handle: Handle,
    _marker: PhantomData<T>,
}

// Common implementations for all Global<T>
impl<T> Global<T> {
    /// # Safety
    /// The caller must ensure that `isolate` and `value` are valid.
    pub unsafe fn from_ffi(isolate: *mut ffi::Isolate, value: usize) -> Self {
        Self {
            handle: unsafe { Handle::new(value, isolate) },
            _marker: PhantomData,
        }
    }
}

impl<T> Clone for Global<T> {
    fn clone(&self) -> Self {
        unsafe {
            Self::from_ffi(
                self.handle.isolate(),
                ffi::clone_global_value(self.handle.isolate(), self.handle.as_ref()),
            )
        }
    }
}

// Value-specific implementations
impl Global<Value> {
    pub fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        unsafe {
            Local::from_ffi(
                lock.get_isolate(),
                ffi::global_to_local_value(lock.get_isolate(), self.clone().handle.to_ffi()),
            )
        }
    }
}

// FunctionTemplate-specific implementations
impl Global<FunctionTemplate> {
    pub fn as_local<'a>(&self, lock: &mut Lock) -> Local<'a, FunctionTemplate> {
        unsafe {
            Local::from_ffi(
                lock.get_isolate(),
                ffi::global_function_template_as_local(lock.get_isolate(), self.handle.as_ref()),
            )
        }
    }
}

pub trait ToLocalValue {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value>;
}

impl ToLocalValue for u8 {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        unsafe {
            Local::from_ffi(
                lock.get_isolate(),
                ffi::new_local_number(lock.get_isolate(), f64::from(*self)),
            )
        }
    }
}

impl ToLocalValue for u32 {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        unsafe {
            Local::from_ffi(
                lock.get_isolate(),
                ffi::new_local_number(lock.get_isolate(), f64::from(*self)),
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
                lock.get_isolate(),
                ffi::new_local_string(lock.get_isolate(), self),
            )
        }
    }
}

pub struct GlobalFunctionTemplate(usize);

impl GlobalFunctionTemplate {
    pub fn as_local<'a>(&self, lock: &mut Lock) -> Local<'a, FunctionTemplate> {
        unsafe {
            Local::from_ffi(
                lock.get_isolate(),
                ffi::global_function_template_as_local(lock.get_isolate(), self.0),
            )
        }
    }

    /// # Safety
    /// The caller must ensure that `value` is a valid FFI representation of a `GlobalFunctionTemplate`.
    pub unsafe fn from_ffi(value: usize) -> Self {
        Self(value)
    }
}

pub struct FunctionCallbackInfo<'a>(*mut ffi::FunctionCallbackInfo, PhantomData<&'a ()>);

impl<'a> FunctionCallbackInfo<'a> {
    /// # Safety
    /// The caller must ensure that `info` is a valid pointer to `FunctionCallbackInfo`.
    pub unsafe fn from_ffi(info: *mut ffi::FunctionCallbackInfo) -> Self {
        Self(info, PhantomData)
    }

    pub fn this(&self, lock: &mut Lock) -> Local<'a, Value> {
        unsafe { Local::from_ffi(lock.get_isolate(), ffi::get_this(self.0)) }
    }

    pub fn len(&self) -> usize {
        unsafe { ffi::get_length(self.0) }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn get(&self, index: usize) -> Local<'a, Value> {
        let isolate = unsafe { ffi::get_isolate(self.0) };
        debug_assert!(index <= self.len());
        unsafe { Local::from_ffi(isolate, ffi::get_arg(self.0, index)) }
    }

    pub fn set_return_value(&self, value: Local<Value>) {
        unsafe {
            ffi::set_return_value(self.0, value.handle.to_ffi());
        }
    }
}
