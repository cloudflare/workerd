//! V8 JavaScript engine bindings and garbage collector integration.
//!
//! This module provides Rust wrappers for V8 types and the cppgc (Oilpan) garbage collector,
//! enabling safe interop between Rust and V8's JavaScript runtime.
//!
//! # Core Types
//!
//! - [`Isolate`] - Safe wrapper around `v8::Isolate*`, the V8 runtime instance
//! - [`Local<'a, T>`] - Stack-allocated handle to a V8 value, tied to a `HandleScope`
//! - [`Global<T>`] - Persistent handle that outlives `HandleScope`s
//! - [`TracedReference<T>`] - Handle traced by the garbage collector for cppgc objects
//!
//! # Garbage Collection
//!
//! The [`cppgc`] submodule provides Rust wrappers for V8's C++ garbage collector types:
//!
//! - [`cppgc::Handle`] - Strong persistent reference (off-heap → on-heap)
//! - [`cppgc::WeakHandle`] - Weak persistent reference (off-heap → on-heap)
//! - [`cppgc::Member`] - Strong member reference (on-heap → on-heap, needs tracing)
//! - [`cppgc::WeakMember`] - Weak member reference (on-heap → on-heap, needs tracing)
//!
//! Resources implement [`GarbageCollected`] to trace their V8 handles during GC.

use std::ffi::c_char;
use std::marker::PhantomData;
use std::ptr::NonNull;

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

    struct TracedReference {
        ptr: usize,
    }

    struct CppgcVisitor {
        ptr: usize,
    }

    enum ExceptionType {
        RangeError,
        ReferenceError,
        SyntaxError,
        TypeError,
        Error,
    }

    extern "Rust" {
        /// Called from C++ RustResource destructor to drop the Rust object.
        /// The `resource` pointer contains a fat pointer (data[2]) to `dyn GarbageCollected`.
        unsafe fn cppgc_invoke_drop(resource: *mut RustResource);

        /// Called from C++ RustResource::Trace to trace nested handles.
        /// The `resource` pointer contains a fat pointer (data[2]) to `dyn GarbageCollected`.
        unsafe fn cppgc_invoke_trace(resource: *const RustResource, visitor: *mut CppgcVisitor);

        /// Called from C++ RustResource::GetHumanReadableName.
        /// Returns null if no name is available, otherwise a static string.
        unsafe fn cppgc_invoke_get_name(resource: *const RustResource) -> *const c_char;
    }

    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type Isolate;
        type FunctionCallbackInfo;
        type RustResource;
        type CppgcPersistent;
        type CppgcWeakPersistent;
        type CppgcMember;
        type CppgcWeakMember;

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
        pub unsafe fn global_reset(value: *mut Global);
        pub unsafe fn global_clone(value: &Global) -> Global;
        pub unsafe fn global_to_local(isolate: *mut Isolate, value: &Global) -> Local;

        // TracedReference<T> (cppgc/Oilpan)
        pub unsafe fn traced_reference_from_local(
            isolate: *mut Isolate,
            value: Local,
        ) -> TracedReference;
        pub unsafe fn traced_reference_to_local(
            isolate: *mut Isolate,
            value: &TracedReference,
        ) -> Local;
        pub unsafe fn traced_reference_reset(value: *mut TracedReference);
        pub unsafe fn traced_reference_is_empty(value: &TracedReference) -> bool;

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

        // cppgc - Allocate Rust objects directly on the GC heap
        pub unsafe fn cppgc_make_garbage_collected(
            isolate: *mut Isolate,
            size: usize,
            alignment: usize,
        ) -> *mut RustResource;
        pub fn cppgc_rust_resource_size() -> usize;
        pub unsafe fn cppgc_rust_resource_data(resource: *mut RustResource) -> *mut usize;
        pub unsafe fn cppgc_rust_resource_data_const(resource: *const RustResource)
        -> *const usize;
        pub unsafe fn cppgc_visitor_trace(visitor: *mut CppgcVisitor, handle: &TracedReference);
        pub unsafe fn cppgc_visitor_trace_member(visitor: *mut CppgcVisitor, member_storage: usize);
        pub unsafe fn cppgc_visitor_trace_weak_member(
            visitor: *mut CppgcVisitor,
            weak_member_storage: usize,
        );
        pub fn cppgc_persistent_size() -> usize;
        pub unsafe fn cppgc_persistent_construct(storage: usize, resource: *mut RustResource);
        pub unsafe fn cppgc_persistent_destruct(storage: usize);
        pub unsafe fn cppgc_persistent_get(storage: usize) -> *mut RustResource;
        pub unsafe fn cppgc_persistent_assign(storage: usize, resource: *mut RustResource);
        // WeakPersistent inline storage functions
        pub fn cppgc_weak_persistent_size() -> usize;
        pub unsafe fn cppgc_weak_persistent_construct(storage: usize, resource: *mut RustResource);
        pub unsafe fn cppgc_weak_persistent_destruct(storage: usize);
        pub unsafe fn cppgc_weak_persistent_get(storage: usize) -> *mut RustResource;
        pub unsafe fn cppgc_weak_persistent_assign(storage: usize, resource: *mut RustResource);
        // Member inline storage functions
        pub fn cppgc_member_size() -> usize;
        pub unsafe fn cppgc_member_construct(storage: usize, resource: *mut RustResource);
        pub unsafe fn cppgc_member_destruct(storage: usize);
        pub unsafe fn cppgc_member_get(storage: usize) -> *mut RustResource;
        pub unsafe fn cppgc_member_assign(storage: usize, resource: *mut RustResource);
        // WeakMember inline storage functions
        pub fn cppgc_weak_member_size() -> usize;
        pub unsafe fn cppgc_weak_member_construct(storage: usize, resource: *mut RustResource);
        pub unsafe fn cppgc_weak_member_destruct(storage: usize);
        pub unsafe fn cppgc_weak_member_get(storage: usize) -> *mut RustResource;
        pub unsafe fn cppgc_weak_member_assign(storage: usize, resource: *mut RustResource);
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
        ) -> Local /* v8::Local<Value> */;

        pub unsafe fn unwrap_resource(
            isolate: *mut Isolate,
            value: Local, /* v8::LocalValue */
        ) -> usize /* R* */;
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
    isolate: Isolate,
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
    pub unsafe fn from_ffi(isolate: Isolate, handle: ffi::Local) -> Self {
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
        unsafe { ffi::local_to_global(lock.isolate().as_ptr(), self.into_ffi()).into() }
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
            ffi::local_object_set_property(
                lock.isolate().as_ptr(),
                &mut self.handle,
                key,
                value.into_ffi(),
            );
        }
    }

    pub fn has(&self, lock: &mut Lock, key: &str) -> bool {
        unsafe { ffi::local_object_has_property(lock.isolate().as_ptr(), &self.handle, key) }
    }

    pub fn get(&self, lock: &mut Lock, key: &str) -> Option<Local<'a, Value>> {
        if !self.has(lock, key) {
            return None;
        }

        unsafe {
            let maybe_local =
                ffi::local_object_get_property(lock.isolate().as_ptr(), &self.handle, key);
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

    /// Returns a mutable reference to the underlying FFI handle.
    ///
    /// # Safety
    /// The caller must ensure the returned reference is not used after this `Global` is dropped.
    pub unsafe fn as_ffi_mut(&mut self) -> *mut ffi::Global {
        &raw mut self.handle
    }

    pub fn as_local<'a>(&self, lock: &mut Lock) -> Local<'a, FunctionTemplate> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::global_to_local(lock.isolate().as_ptr(), &self.handle),
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
            handle: unsafe { ffi::local_to_global(local.isolate.as_ptr(), local.into_ffi()) },
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

impl ToLocalValue for u8 {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_new_number(lock.isolate().as_ptr(), f64::from(*self)),
            )
        }
    }
}

impl ToLocalValue for u32 {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_new_number(lock.isolate().as_ptr(), f64::from(*self)),
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
                ffi::local_new_string(lock.isolate().as_ptr(), self),
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

    pub fn isolate(&self) -> Isolate {
        unsafe { Isolate::from_raw(ffi::fci_get_isolate(self.0)) }
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

/// A reference to a V8 value that is traced by the garbage collector.
///
/// Note: This type intentionally does NOT implement `Drop`. `TracedReference` is managed by V8's
/// traced handles infrastructure. During cppgc finalization, the traced handle memory may already
/// be freed, so calling `reset()` would cause a use-after-free. V8 automatically cleans up traced
/// references during GC.
pub struct TracedReference<T> {
    handle: ffi::TracedReference,
    _marker: PhantomData<T>,
}

impl<T> TracedReference<T> {
    pub fn is_empty(&self) -> bool {
        unsafe { ffi::traced_reference_is_empty(&self.handle) }
    }

    pub fn get<'a>(&self, lock: &mut Lock) -> Local<'a, T> {
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::traced_reference_to_local(lock.isolate().as_ptr(), &self.handle),
            )
        }
    }

    pub fn reset(&mut self) {
        unsafe {
            ffi::traced_reference_reset(&raw mut self.handle);
        }
    }

    /// # Safety
    /// The returned reference must not outlive this handle.
    pub unsafe fn as_ffi_ref(&self) -> &ffi::TracedReference {
        &self.handle
    }
}

impl<T> From<Local<'_, T>> for TracedReference<T> {
    fn from(local: Local<'_, T>) -> Self {
        Self {
            handle: unsafe {
                ffi::traced_reference_from_local(local.isolate.as_ptr(), local.into_ffi())
            },
            _marker: PhantomData,
        }
    }
}

/// A fat pointer to a `dyn GarbageCollected` trait object.
///
/// This struct has the same memory layout as a Rust fat pointer: `[data_ptr, vtable_ptr]`.
/// It's used to store trait object pointers in C++ `RustResource::data[2]` and safely
/// reconstruct them on the Rust side.
///
/// Uses `NonNull` internally to enforce that the pointers are never null.
#[repr(C)]
struct TraitObjectPtr {
    data: NonNull<()>,
    vtable: NonNull<()>,
}

impl TraitObjectPtr {
    /// Creates a `TraitObjectPtr` from a mutable reference to any `GarbageCollected` type.
    fn from_ref(obj: &mut dyn GarbageCollected) -> Self {
        // SAFETY: Transmute is safe here because we're converting a fat pointer to its raw
        // representation which is guaranteed to be [data_ptr, vtable_ptr] for trait objects.
        // References are always non-null, so NonNull is valid.
        let [data, vtable] =
            unsafe { std::mem::transmute::<*mut dyn GarbageCollected, [NonNull<()>; 2]>(obj) };
        Self { data, vtable }
    }

    /// Reconstructs a shared reference from the stored fat pointer.
    ///
    /// # Safety
    /// The original object must still be alive for lifetime `'a`.
    #[expect(clippy::needless_lifetimes)]
    unsafe fn as_ref<'a>(&'a self) -> &'a dyn GarbageCollected {
        let fat_ptr = unsafe {
            std::mem::transmute::<[NonNull<()>; 2], *const dyn GarbageCollected>([
                self.data,
                self.vtable,
            ])
        };
        unsafe { &*fat_ptr }
    }

    /// Reconstructs a mutable reference from the stored fat pointer.
    ///
    /// # Safety
    /// The original object must still be alive for lifetime `'a`, and no other references may exist.
    #[expect(clippy::needless_lifetimes)]
    unsafe fn as_mut<'a>(&'a mut self) -> &'a mut dyn GarbageCollected {
        let fat_ptr = unsafe {
            std::mem::transmute::<[NonNull<()>; 2], *mut dyn GarbageCollected>([
                self.data,
                self.vtable,
            ])
        };
        unsafe { &mut *fat_ptr }
    }
}

/// Returns a reference to the `TraitObjectPtr` stored in a `RustResource`.
///
/// # Safety
/// The resource pointer must be valid and contain a properly initialized `TraitObjectPtr`.
unsafe fn get_trait_object_ptr(resource: *const ffi::RustResource) -> &'static TraitObjectPtr {
    let data_ptr = unsafe { ffi::cppgc_rust_resource_data_const(resource) };
    unsafe { &*data_ptr.cast::<TraitObjectPtr>() }
}

/// Returns a mutable reference to the `TraitObjectPtr` stored in a `RustResource`.
///
/// # Safety
/// The resource pointer must be valid and contain a properly initialized `TraitObjectPtr`.
unsafe fn get_trait_object_ptr_mut(
    resource: *mut ffi::RustResource,
) -> &'static mut TraitObjectPtr {
    let data_ptr = unsafe { ffi::cppgc_rust_resource_data(resource) };
    unsafe { &mut *data_ptr.cast::<TraitObjectPtr>() }
}

unsafe fn cppgc_invoke_drop(resource: *mut ffi::RustResource) {
    let trait_ptr = unsafe { get_trait_object_ptr_mut(resource) };
    let obj = unsafe { trait_ptr.as_mut() };
    unsafe { std::ptr::drop_in_place(obj) };
}

unsafe fn cppgc_invoke_trace(resource: *const ffi::RustResource, visitor: *mut ffi::CppgcVisitor) {
    let trait_ptr = unsafe { get_trait_object_ptr(resource) };
    let obj = unsafe { trait_ptr.as_ref() };
    let mut gc_visitor = unsafe {
        GcVisitor::from_raw(ffi::CppgcVisitor {
            ptr: (*visitor).ptr,
        })
    };
    obj.trace(&mut gc_visitor);
}

unsafe fn cppgc_invoke_get_name(resource: *const ffi::RustResource) -> *const c_char {
    let trait_ptr = unsafe { get_trait_object_ptr(resource) };
    let obj = unsafe { trait_ptr.as_ref() };
    obj.get_name()
        .map_or(std::ptr::null(), std::ffi::CStr::as_ptr)
}

pub mod cppgc {
    use std::cell::UnsafeCell;
    use std::marker::PhantomData;
    use std::ptr::NonNull;

    use super::GarbageCollected;
    use super::Isolate;
    use super::ffi;

    const PERSISTENT_USIZE_COUNT: usize = 2;
    const WEAK_PERSISTENT_USIZE_COUNT: usize = 2;
    const MEMBER_USIZE_COUNT: usize = 1;
    const WEAK_MEMBER_USIZE_COUNT: usize = 1;

    fn object_offset_for<T>() -> usize {
        let base = ffi::cppgc_rust_resource_size();
        let align = std::mem::align_of::<T>();
        (base + align - 1) & !(align - 1)
    }

    unsafe fn get_object_from_rust_resource<T: GarbageCollected>(
        rust_resource: *const ffi::RustResource,
    ) -> *mut T {
        unsafe {
            rust_resource
                .cast::<u8>()
                .add(object_offset_for::<T>())
                .cast::<T>()
                .cast_mut()
        }
    }

    /// # Safety
    /// The isolate must be valid and locked by the current thread.
    ///
    /// # Panics
    /// Panics if allocation fails (null pointer returned from cppgc).
    pub unsafe fn make_garbage_collected<T: GarbageCollected + 'static>(
        isolate: Isolate,
        obj: T,
    ) -> Ptr<T> {
        const {
            assert!(std::mem::align_of::<T>() <= 16);
        }

        let base_size = ffi::cppgc_rust_resource_size();
        let aligned_offset = object_offset_for::<T>();
        let additional_bytes = (aligned_offset - base_size) + std::mem::size_of::<T>();

        let pointer = unsafe {
            ffi::cppgc_make_garbage_collected(
                isolate.as_ptr(),
                additional_bytes,
                std::mem::align_of::<T>(),
            )
        };

        assert!(!pointer.is_null(), "cppgc allocation failed");

        unsafe {
            let inner = get_object_from_rust_resource::<T>(pointer);
            inner.write(obj);

            // Store the fat pointer (data + vtable) in RustResource::data[2]
            let data_ptr = ffi::cppgc_rust_resource_data(pointer);
            let trait_ptr = super::TraitObjectPtr::from_ref(&mut *inner);
            std::ptr::write(data_ptr.cast::<super::TraitObjectPtr>(), trait_ptr);
        }

        Ptr {
            pointer: unsafe { NonNull::new_unchecked(pointer) },
            _phantom: PhantomData,
        }
    }

    /// # Safety
    /// `rust_resource` must point to a valid `RustResource` containing an object of type T.
    pub unsafe fn rust_resource_to_instance<T: GarbageCollected>(
        rust_resource: *mut ffi::RustResource,
    ) -> *mut T {
        unsafe { get_object_from_rust_resource::<T>(rust_resource) }
    }

    /// # Safety
    /// `instance` must point to a valid object allocated via `make_garbage_collected`.
    pub unsafe fn instance_to_rust_resource<T: GarbageCollected>(
        instance: *mut T,
    ) -> *mut ffi::RustResource {
        let offset = object_offset_for::<T>();
        unsafe {
            instance
                .cast::<u8>()
                .sub(offset)
                .cast::<ffi::RustResource>()
        }
    }

    #[derive(Clone, Copy)]
    pub struct Ptr<T: GarbageCollected> {
        pointer: NonNull<ffi::RustResource>,
        _phantom: PhantomData<T>,
    }

    impl<T: GarbageCollected> Ptr<T> {
        pub fn get(&self) -> &T {
            unsafe { &*get_object_from_rust_resource(self.pointer.as_ptr()) }
        }

        pub fn as_rust_resource(&self) -> *mut ffi::RustResource {
            self.pointer.as_ptr()
        }
    }

    impl<T: GarbageCollected> std::ops::Deref for Ptr<T> {
        type Target = T;

        fn deref(&self) -> &T {
            self.get()
        }
    }

    impl<T: GarbageCollected + std::fmt::Debug> std::fmt::Debug for Ptr<T> {
        fn fmt(&self, fmt: &mut std::fmt::Formatter) -> std::fmt::Result {
            std::fmt::Debug::fmt(&**self, fmt)
        }
    }

    // cppgc handles cannot be bitwise moved after construction due to internal
    // back-pointers. We use Box to ensure stable addresses.

    struct PersistentInner(Box<[usize; PERSISTENT_USIZE_COUNT]>);

    impl PersistentInner {
        fn new(resource: *mut ffi::RustResource) -> Self {
            let mut storage = Box::new([0usize; PERSISTENT_USIZE_COUNT]);
            unsafe { ffi::cppgc_persistent_construct(storage.as_mut_ptr() as usize, resource) }
            Self(storage)
        }

        fn get(&self) -> *mut ffi::RustResource {
            unsafe { ffi::cppgc_persistent_get(self.0.as_ptr() as usize) }
        }
    }

    impl Drop for PersistentInner {
        fn drop(&mut self) {
            unsafe { ffi::cppgc_persistent_destruct(self.0.as_mut_ptr() as usize) }
        }
    }

    pub struct Handle {
        inner: UnsafeCell<Option<PersistentInner>>,
    }

    impl Default for Handle {
        fn default() -> Self {
            Self::new()
        }
    }

    impl Handle {
        pub fn new() -> Self {
            Self {
                inner: UnsafeCell::new(None),
            }
        }

        /// # Safety
        /// The resource pointer must be valid and allocated via `make_garbage_collected`.
        pub unsafe fn from_resource(resource: *mut ffi::RustResource) -> Self {
            Self {
                inner: UnsafeCell::new(Some(PersistentInner::new(resource))),
            }
        }

        pub fn has_persistent(&self) -> bool {
            // SAFETY: V8 isolate is single-threaded
            unsafe { (*self.inner.get()).is_some() }
        }

        pub fn get_resource(&self) -> Option<NonNull<ffi::RustResource>> {
            // SAFETY: V8 isolate is single-threaded
            unsafe {
                (*self.inner.get())
                    .as_ref()
                    .and_then(|p| NonNull::new(p.get()))
            }
        }

        /// Clears the persistent handle, releasing the GC root.
        /// This enables cycle collection for traced refs.
        pub fn clear(&self) {
            // SAFETY: V8 isolate is single-threaded
            unsafe {
                (*self.inner.get()).take();
            }
        }

        pub fn release(&mut self) {
            // SAFETY: We have &mut self
            unsafe {
                (*self.inner.get()).take();
            }
        }
    }

    struct WeakPersistentInner(Box<[usize; WEAK_PERSISTENT_USIZE_COUNT]>);

    impl WeakPersistentInner {
        fn new(resource: *mut ffi::RustResource) -> Self {
            let mut storage = Box::new([0usize; WEAK_PERSISTENT_USIZE_COUNT]);
            unsafe { ffi::cppgc_weak_persistent_construct(storage.as_mut_ptr() as usize, resource) }
            Self(storage)
        }

        fn get(&self) -> *mut ffi::RustResource {
            unsafe { ffi::cppgc_weak_persistent_get(self.0.as_ptr() as usize) }
        }
    }

    impl Drop for WeakPersistentInner {
        fn drop(&mut self) {
            unsafe { ffi::cppgc_weak_persistent_destruct(self.0.as_mut_ptr() as usize) }
        }
    }

    #[derive(Default)]
    pub struct WeakHandle {
        inner: Option<WeakPersistentInner>,
    }

    impl WeakHandle {
        /// # Safety
        /// The resource pointer must be valid and allocated via `make_garbage_collected`.
        pub unsafe fn from_resource(resource: *mut ffi::RustResource) -> Self {
            Self {
                inner: Some(WeakPersistentInner::new(resource)),
            }
        }

        pub fn get(&self) -> Option<NonNull<ffi::RustResource>> {
            self.inner.as_ref().and_then(|p| NonNull::new(p.get()))
        }

        pub fn is_alive(&self) -> bool {
            self.get().is_some()
        }
    }

    pub(super) struct MemberInner(Box<UnsafeCell<[usize; MEMBER_USIZE_COUNT]>>);

    impl MemberInner {
        fn new(resource: *mut ffi::RustResource) -> Self {
            let storage = Box::new(UnsafeCell::new([0usize; MEMBER_USIZE_COUNT]));
            unsafe { ffi::cppgc_member_construct(storage.get() as usize, resource) }
            Self(storage)
        }

        fn get(&self) -> *mut ffi::RustResource {
            unsafe { ffi::cppgc_member_get(self.0.get() as usize) }
        }

        fn assign(&self, resource: *mut ffi::RustResource) {
            unsafe { ffi::cppgc_member_assign(self.0.get() as usize, resource) }
        }

        pub(super) fn as_usize(&self) -> usize {
            self.0.get() as usize
        }
    }

    impl Drop for MemberInner {
        fn drop(&mut self) {
            unsafe { ffi::cppgc_member_destruct(self.0.get() as usize) }
        }
    }

    pub struct Member {
        pub(super) inner: UnsafeCell<Option<MemberInner>>,
    }

    impl Default for Member {
        fn default() -> Self {
            Self::new()
        }
    }

    impl Member {
        pub fn new() -> Self {
            Self {
                inner: UnsafeCell::new(None),
            }
        }

        pub fn from_resource(resource: NonNull<ffi::RustResource>) -> Self {
            Self {
                inner: UnsafeCell::new(Some(MemberInner::new(resource.as_ptr()))),
            }
        }

        pub fn get(&self) -> Option<NonNull<ffi::RustResource>> {
            // SAFETY: V8 isolate is single-threaded, so no concurrent access is possible.
            unsafe {
                (*self.inner.get())
                    .as_ref()
                    .and_then(|m| NonNull::new(m.get()))
            }
        }

        pub fn set(&self, resource: Option<NonNull<ffi::RustResource>>) {
            let resource_ptr = resource.map_or(std::ptr::null_mut(), NonNull::as_ptr);
            // SAFETY: V8 isolate is single-threaded, so no concurrent access is possible.
            unsafe {
                if let Some(ref m) = *self.inner.get() {
                    m.assign(resource_ptr);
                } else if let Some(r) = resource {
                    *self.inner.get() = Some(MemberInner::new(r.as_ptr()));
                }
            }
        }
    }

    pub(super) struct WeakMemberInner(Box<UnsafeCell<[usize; WEAK_MEMBER_USIZE_COUNT]>>);

    impl WeakMemberInner {
        fn new(resource: *mut ffi::RustResource) -> Self {
            let storage = Box::new(UnsafeCell::new([0usize; WEAK_MEMBER_USIZE_COUNT]));
            unsafe { ffi::cppgc_weak_member_construct(storage.get() as usize, resource) }
            Self(storage)
        }

        fn get(&self) -> *mut ffi::RustResource {
            unsafe { ffi::cppgc_weak_member_get(self.0.get() as usize) }
        }

        fn assign(&self, resource: *mut ffi::RustResource) {
            unsafe { ffi::cppgc_weak_member_assign(self.0.get() as usize, resource) }
        }

        pub(super) fn as_usize(&self) -> usize {
            self.0.get() as usize
        }
    }

    impl Drop for WeakMemberInner {
        fn drop(&mut self) {
            unsafe { ffi::cppgc_weak_member_destruct(self.0.get() as usize) }
        }
    }

    pub struct WeakMember {
        pub(super) inner: UnsafeCell<Option<WeakMemberInner>>,
    }

    impl Default for WeakMember {
        fn default() -> Self {
            Self::new()
        }
    }

    impl WeakMember {
        pub fn new() -> Self {
            Self {
                inner: UnsafeCell::new(None),
            }
        }

        pub fn from_resource(resource: NonNull<ffi::RustResource>) -> Self {
            Self {
                inner: UnsafeCell::new(Some(WeakMemberInner::new(resource.as_ptr()))),
            }
        }

        pub fn get(&self) -> Option<NonNull<ffi::RustResource>> {
            // SAFETY: V8 isolate is single-threaded, so no concurrent access is possible.
            unsafe {
                (*self.inner.get())
                    .as_ref()
                    .and_then(|m| NonNull::new(m.get()))
            }
        }

        pub fn is_alive(&self) -> bool {
            // SAFETY: V8 isolate is single-threaded, so no concurrent access is possible.
            unsafe { (*self.inner.get()).is_some() }
        }

        pub fn set(&self, resource: Option<NonNull<ffi::RustResource>>) {
            let resource_ptr = resource.map_or(std::ptr::null_mut(), NonNull::as_ptr);
            // SAFETY: V8 isolate is single-threaded, so no concurrent access is possible.
            unsafe {
                if let Some(ref m) = *self.inner.get() {
                    m.assign(resource_ptr);
                } else if let Some(r) = resource {
                    *self.inner.get() = Some(WeakMemberInner::new(r.as_ptr()));
                }
            }
        }
    }

    #[cfg(test)]
    mod tests {
        use super::*;

        #[test]
        fn handle_default_is_empty() {
            let handle = Handle::new();
            assert!(!handle.has_persistent());
            assert!(handle.get_resource().is_none());
        }

        #[test]
        fn member_default_is_empty() {
            let member = Member::new();
            assert!(member.get().is_none());
        }

        #[test]
        fn weak_member_default_is_empty() {
            let weak_member = WeakMember::new();
            assert!(!weak_member.is_alive());
            assert!(weak_member.get().is_none());
        }
    }
}

pub trait GarbageCollected {
    fn trace(&self, _visitor: &mut GcVisitor<'_>) {}

    fn get_name(&self) -> Option<&'static std::ffi::CStr> {
        None
    }
}

/// Trait for accessing parent context during GC visitation.
///
/// This is implemented by `Instance<R>` to provide context for dynamic
/// strong/traced switching of `Ref`s.
pub trait GcParent {
    /// Returns the current strong reference count.
    fn strong_refcount(&self) -> u32;

    /// Returns whether this instance has a JS wrapper.
    fn has_wrapper(&self) -> bool;
}

/// Visitor for garbage collection tracing.
///
/// `GcVisitor` wraps the cppgc visitor and tracks parent context for dynamic
/// strong/traced switching of `Ref`s.
pub struct GcVisitor<'a> {
    visitor: ffi::CppgcVisitor,
    /// Reference to the parent being traced (used for dynamic Ref switching)
    parent: Option<&'a dyn GcParent>,
}

impl GcVisitor<'_> {
    /// Creates a new `GcVisitor` from a raw cppgc visitor.
    ///
    /// # Safety
    /// The visitor must be valid for the lifetime of this `GcVisitor`.
    pub unsafe fn from_raw(visitor: ffi::CppgcVisitor) -> Self {
        Self {
            visitor,
            parent: None,
        }
    }

    /// Creates a child visitor with the given parent context.
    ///
    /// Use this when tracing through a resource to provide context for
    /// dynamic strong/traced switching of child `Ref`s.
    pub fn with_parent<'b>(&mut self, parent: &'b dyn GcParent) -> GcVisitor<'b> {
        GcVisitor {
            visitor: ffi::CppgcVisitor {
                ptr: self.visitor.ptr,
            },
            parent: Some(parent),
        }
    }

    /// Returns the parent context if available.
    pub fn parent(&self) -> Option<&dyn GcParent> {
        self.parent
    }

    pub fn trace<T>(&mut self, handle: &TracedReference<T>) {
        unsafe {
            ffi::cppgc_visitor_trace(std::ptr::from_mut(&mut self.visitor), handle.as_ffi_ref());
        }
    }

    pub fn trace_member(&mut self, member: &cppgc::Member) {
        // SAFETY: V8 isolate is single-threaded, so no concurrent access is possible.
        // We're in a GC trace callback, so the member is guaranteed to be valid.
        if let Some(m) = unsafe { &*member.inner.get() } {
            unsafe {
                ffi::cppgc_visitor_trace_member(
                    std::ptr::from_mut(&mut self.visitor),
                    m.as_usize(),
                );
            }
        }
    }

    pub fn trace_weak_member(&mut self, member: &cppgc::WeakMember) {
        // SAFETY: V8 isolate is single-threaded, so no concurrent access is possible.
        // We're in a GC trace callback, so the member is guaranteed to be valid.
        if let Some(m) = unsafe { &*member.inner.get() } {
            unsafe {
                ffi::cppgc_visitor_trace_weak_member(
                    std::ptr::from_mut(&mut self.visitor),
                    m.as_usize(),
                );
            }
        }
    }

    /// Visits a `Ref` during GC tracing, handling dynamic strong/traced switching.
    ///
    /// This method:
    /// 1. Calls `ref.visit()` to potentially switch the ref between strong and traced,
    ///    and trace the Member storage if in traced mode
    /// 2. Traces the target's wrapper if it exists
    /// 3. If no wrapper, recursively traces through the target's children
    ///
    /// This mirrors the behavior of `Wrappable::visitRef` in C++.
    pub fn visit_ref<R: crate::Resource>(&mut self, r: &crate::Ref<R>) {
        let instance = r.visit(self);

        if let Some(wrapper) = instance.traced_reference() {
            // Target has a wrapper - trace it
            self.trace(wrapper);
        } else {
            // No wrapper - trace transitively through the target's children
            let mut child_visitor = self.with_parent(instance);
            instance.resource.trace(&mut child_visitor);
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Isolate {
    handle: NonNull<ffi::Isolate>,
}

impl Isolate {
    /// # Safety
    /// The pointer must be non-null and point to a valid V8 isolate.
    pub unsafe fn from_raw(handle: *mut ffi::Isolate) -> Self {
        debug_assert!(unsafe { ffi::isolate_is_locked(handle) });
        Self {
            handle: unsafe { NonNull::new_unchecked(handle) },
        }
    }

    pub fn from_non_null(handle: NonNull<ffi::Isolate>) -> Self {
        debug_assert!(unsafe { ffi::isolate_is_locked(handle.as_ptr()) });
        Self { handle }
    }

    pub fn is_locked(&self) -> bool {
        unsafe { ffi::isolate_is_locked(self.handle.as_ptr()) }
    }

    pub fn as_ptr(&self) -> *mut ffi::Isolate {
        self.handle.as_ptr()
    }

    pub fn as_non_null(&self) -> NonNull<ffi::Isolate> {
        self.handle
    }
}
