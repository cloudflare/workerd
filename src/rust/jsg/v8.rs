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

use core::ffi::c_void;
use std::marker::PhantomData;
use std::ptr::NonNull;

use crate::Lock;

#[expect(clippy::missing_safety_doc, clippy::missing_panics_doc)]
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

    /// Data stored in a cppgc-managed RustResource object.
    /// This struct is shared between Rust and C++ via CXX.
    struct RustResourceData {
        /// Pointer to the Rust resource instance
        instance_ptr: usize,
        /// Function pointer to drop the instance: fn(*mut c_void)
        drop_fn: usize,
        /// Function pointer to trace the instance: fn(*mut c_void, *mut CppgcVisitor)
        trace_fn: usize,
    }

    extern "Rust" {
        /// Called from C++ RustResource destructor to drop the Rust instance.
        unsafe fn cppgc_invoke_drop(data: &RustResourceData);

        /// Called from C++ RustResource::Trace to trace nested handles.
        unsafe fn cppgc_invoke_trace(data: &RustResourceData, visitor: *mut CppgcVisitor);
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

        // cppgc
        pub unsafe fn cppgc_allocate(
            isolate: *mut Isolate,
            data: RustResourceData,
        ) -> *mut RustResource;
        pub unsafe fn cppgc_visitor_trace(visitor: *mut CppgcVisitor, handle: &TracedReference);
        pub unsafe fn cppgc_visitor_trace_member(visitor: *mut CppgcVisitor, member: &CppgcMember);
        pub unsafe fn cppgc_visitor_trace_weak_member(
            visitor: *mut CppgcVisitor,
            member: &CppgcWeakMember,
        );
        pub unsafe fn cppgc_persistent_new(resource: *mut RustResource) -> KjOwn<CppgcPersistent>;
        pub unsafe fn cppgc_persistent_get(persistent: &CppgcPersistent) -> *mut RustResource;
        pub unsafe fn cppgc_weak_persistent_new(
            resource: *mut RustResource,
        ) -> KjOwn<CppgcWeakPersistent>;
        pub unsafe fn cppgc_weak_persistent_get(
            persistent: &CppgcWeakPersistent,
        ) -> *mut RustResource;
        pub unsafe fn cppgc_member_new(resource: *mut RustResource) -> KjOwn<CppgcMember>;
        pub unsafe fn cppgc_member_get(member: &CppgcMember) -> *mut RustResource;
        pub unsafe fn cppgc_member_set(member: Pin<&mut CppgcMember>, resource: *mut RustResource);
        pub unsafe fn cppgc_weak_member_new(resource: *mut RustResource) -> KjOwn<CppgcWeakMember>;
        pub unsafe fn cppgc_weak_member_get(member: &CppgcWeakMember) -> *mut RustResource;
        pub unsafe fn cppgc_weak_member_set(
            member: Pin<&mut CppgcWeakMember>,
            resource: *mut RustResource,
        );
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

// ============================================================================
// cppgc callbacks
// ============================================================================

/// Called from C++ `RustResource` destructor to drop the Rust instance.
///
/// # Safety
/// The `data` must contain valid pointers.
unsafe fn cppgc_invoke_drop(data: &ffi::RustResourceData) {
    debug_assert!(data.instance_ptr != 0, "instance_ptr must be valid");
    debug_assert!(data.drop_fn != 0, "drop_fn must be valid");
    let drop_fn: unsafe fn(*mut c_void) = unsafe { std::mem::transmute(data.drop_fn) };
    unsafe { drop_fn(data.instance_ptr as *mut c_void) };
}

/// Called from C++ `RustResource::Trace` to trace nested handles.
///
/// # Safety
/// The `data` must contain valid pointers, and `visitor` must be valid.
unsafe fn cppgc_invoke_trace(data: &ffi::RustResourceData, visitor: *mut ffi::CppgcVisitor) {
    debug_assert!(data.instance_ptr != 0, "instance_ptr must be valid");
    if data.trace_fn == 0 {
        return;
    }
    let trace_fn: unsafe fn(*mut c_void, *mut ffi::CppgcVisitor) =
        unsafe { std::mem::transmute(data.trace_fn) };
    unsafe { trace_fn(data.instance_ptr as *mut c_void, visitor) };
}

// ============================================================================
// cppgc module - V8's C++ garbage collector integration
// ============================================================================

/// Rust wrappers for cppgc (Oilpan) garbage collector types.
///
/// This module provides safe abstractions over V8's cppgc types for managing
/// garbage-collected references to `RustResource` objects.
///
/// # Reference Types
///
/// | Type | C++ Equivalent | Description |
/// |------|----------------|-------------|
/// | [`Handle`] | `cppgc::Persistent<T>` | Strong off-heap → on-heap reference |
/// | [`WeakHandle`] | `cppgc::WeakPersistent<T>` | Weak off-heap → on-heap reference |
/// | [`Member`] | `cppgc::Member<T>` | Strong on-heap → on-heap reference |
/// | [`WeakMember`] | `cppgc::WeakMember<T>` | Weak on-heap → on-heap reference |
///
/// # Tracing
///
/// Off-heap references (`Handle`, `WeakHandle`) do not need to be traced because
/// they are persistent handles managed by cppgc. On-heap references (`Member`,
/// `WeakMember`) must be traced during garbage collection via [`GcVisitor`].
///
/// [`GcVisitor`]: super::GcVisitor
pub mod cppgc {
    use std::ptr::NonNull;

    use kj_rs::KjOwn;

    use super::ffi;

    // ========================================================================
    // Handle - Strong persistent reference (off-heap → on-heap)
    // ========================================================================

    /// Strong persistent reference to a `RustResource`.
    ///
    /// Wraps `cppgc::Persistent<RustResource>` which keeps the resource alive
    /// while Rust has `Ref<R>` handles. Used for off-heap → on-heap references.
    /// Automatically releases when dropped via `KjOwn`.
    #[derive(Default)]
    pub struct Handle {
        persistent: Option<KjOwn<ffi::CppgcPersistent>>,
    }

    impl Handle {
        /// Creates a new empty `Handle`.
        pub fn new() -> Self {
            Self { persistent: None }
        }

        /// Creates a `Handle` from a `RustResource` pointer.
        ///
        /// # Safety
        /// The resource pointer must be valid and allocated via `cppgc_allocate`.
        pub unsafe fn from_resource(resource: *mut ffi::RustResource) -> Self {
            Self {
                persistent: Some(unsafe { ffi::cppgc_persistent_new(resource) }),
            }
        }

        /// Returns whether this handle has a persistent reference.
        pub fn has_persistent(&self) -> bool {
            self.persistent.is_some()
        }

        /// Returns the `RustResource` pointer if this handle has a persistent reference.
        pub fn get_resource(&self) -> Option<NonNull<ffi::RustResource>> {
            self.persistent
                .as_ref()
                .and_then(|p| NonNull::new(unsafe { ffi::cppgc_persistent_get(p) }))
        }

        /// Releases the persistent handle, allowing cppgc to garbage collect.
        pub fn release(&mut self) {
            self.persistent.take();
        }
    }

    // ========================================================================
    // WeakHandle - Weak persistent reference (off-heap → on-heap)
    // ========================================================================

    /// Weak persistent reference to a `RustResource`.
    ///
    /// Wraps `cppgc::WeakPersistent<RustResource>` which doesn't prevent GC.
    /// Used for off-heap → on-heap weak references that don't need tracing.
    /// Automatically releases when dropped via `KjOwn`.
    pub struct WeakHandle {
        handle: Option<KjOwn<ffi::CppgcWeakPersistent>>,
    }

    impl WeakHandle {
        /// Creates a `WeakHandle` from a `RustResource` pointer.
        ///
        /// # Safety
        /// The resource pointer must be valid and allocated via `cppgc_allocate`.
        pub unsafe fn from_resource(resource: *mut ffi::RustResource) -> Self {
            Self {
                handle: Some(unsafe { ffi::cppgc_weak_persistent_new(resource) }),
            }
        }

        /// Returns the `RustResource` pointer if the target is still alive.
        pub fn get(&self) -> Option<NonNull<ffi::RustResource>> {
            self.handle
                .as_ref()
                .and_then(|p| NonNull::new(unsafe { ffi::cppgc_weak_persistent_get(p) }))
        }

        /// Returns whether the weak handle points to a live resource.
        pub fn is_alive(&self) -> bool {
            self.get().is_some()
        }
    }

    // ========================================================================
    // Member - Traceable strong reference (on-heap → on-heap)
    // ========================================================================

    /// Traceable strong reference to a `RustResource`.
    ///
    /// Wraps `cppgc::Member<RustResource>` for strong references stored inside
    /// GC'd objects. Must be traced via `GcVisitor::trace_member()`.
    /// Automatically releases when dropped via `KjOwn`.
    #[derive(Default)]
    pub struct Member {
        pub(super) handle: Option<KjOwn<ffi::CppgcMember>>,
    }

    impl Member {
        /// Creates a new empty `Member`.
        pub fn new() -> Self {
            Self { handle: None }
        }

        /// Creates a `Member` from a `RustResource` pointer.
        pub fn from_resource(resource: NonNull<ffi::RustResource>) -> Self {
            Self {
                handle: Some(unsafe { ffi::cppgc_member_new(resource.as_ptr()) }),
            }
        }

        /// Returns the `RustResource` pointer if set.
        pub fn get(&self) -> Option<NonNull<ffi::RustResource>> {
            self.handle
                .as_ref()
                .and_then(|m| NonNull::new(unsafe { ffi::cppgc_member_get(m) }))
        }

        /// Sets the target resource.
        pub fn set(&mut self, resource: Option<NonNull<ffi::RustResource>>) {
            let resource_ptr = resource.map_or(std::ptr::null_mut(), NonNull::as_ptr);
            if let Some(ref mut m) = self.handle {
                unsafe { ffi::cppgc_member_set(m.as_mut(), resource_ptr) };
            } else if let Some(r) = resource {
                self.handle = Some(unsafe { ffi::cppgc_member_new(r.as_ptr()) });
            }
        }
    }

    // ========================================================================
    // WeakMember - Traceable weak reference (on-heap → on-heap)
    // ========================================================================

    /// Traceable weak reference to a `RustResource`.
    ///
    /// Wraps `cppgc::WeakMember<RustResource>` for weak references stored inside
    /// GC'd objects. Must be traced via `GcVisitor::trace_weak_member()`.
    /// When the target is collected, `get()` returns `None`.
    /// Automatically releases when dropped via `KjOwn`.
    #[derive(Default)]
    pub struct WeakMember {
        pub(super) handle: Option<KjOwn<ffi::CppgcWeakMember>>,
    }

    impl WeakMember {
        /// Creates a new empty `WeakMember`.
        pub fn new() -> Self {
            Self { handle: None }
        }

        /// Creates a `WeakMember` from a `RustResource` pointer.
        pub fn from_resource(resource: NonNull<ffi::RustResource>) -> Self {
            Self {
                handle: Some(unsafe { ffi::cppgc_weak_member_new(resource.as_ptr()) }),
            }
        }

        /// Returns the `RustResource` pointer if the target is still alive.
        pub fn get(&self) -> Option<NonNull<ffi::RustResource>> {
            self.handle.as_ref().and_then(|m| {
                // SAFETY: m.as_ref() returns a valid reference to the CppgcWeakMember
                NonNull::new(unsafe { ffi::cppgc_weak_member_get(m.as_ref()) })
            })
        }

        /// Returns whether the weak member points to a live resource.
        pub fn is_alive(&self) -> bool {
            self.handle.is_some()
        }

        /// Sets the target resource.
        pub fn set(&mut self, resource: Option<NonNull<ffi::RustResource>>) {
            let resource_ptr = resource.map_or(std::ptr::null_mut(), NonNull::as_ptr);
            if let Some(ref mut m) = self.handle {
                unsafe { ffi::cppgc_weak_member_set(m.as_mut(), resource_ptr) };
            } else if let Some(r) = resource {
                self.handle = Some(unsafe { ffi::cppgc_weak_member_new(r.as_ptr()) });
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

// ============================================================================
// GarbageCollected trait and GcVisitor
// ============================================================================

/// Trait for types that participate in V8's cppgc garbage collection.
///
/// Resources that hold references to JavaScript values must implement this trait
/// to trace those references during garbage collection. This allows the GC to
/// properly track the object graph and prevent premature collection.
///
/// # What to Trace
///
/// - `TracedReference<T>` - JavaScript handle stored in a cppgc object
/// - `cppgc::Member` - Strong on-heap reference to another cppgc object
/// - `cppgc::WeakMember` - Weak on-heap reference to another cppgc object
///
/// # Example
///
/// ```ignore
/// impl GarbageCollected for MyResource {
///     fn trace(&self, visitor: &mut GcVisitor) {
///         // Trace JavaScript handles
///         if let Some(ref callback) = self.js_callback {
///             visitor.trace(callback);
///         }
///         // Trace weak members
///         visitor.trace_weak_member(&self.weak_ref);
///     }
/// }
/// ```
pub trait GarbageCollected {
    /// Traces any JavaScript handles held by this object.
    ///
    /// Called by the garbage collector during tracing. Implementations should
    /// call the appropriate `visitor` methods for each traced field.
    fn trace(&self, _visitor: &mut GcVisitor) {}
}

/// Visitor for tracing garbage-collected references.
///
/// Passed to [`GarbageCollected::trace`] to report which objects are reachable
/// from the current object. The garbage collector uses this information to
/// determine which objects are still alive.
pub struct GcVisitor {
    visitor: ffi::CppgcVisitor,
}

impl GcVisitor {
    /// # Safety
    /// The visitor must be valid for the lifetime of this `GcVisitor`.
    pub unsafe fn from_raw(visitor: ffi::CppgcVisitor) -> Self {
        Self { visitor }
    }

    /// Traces a `TracedReference` handle.
    pub fn trace<T>(&mut self, handle: &TracedReference<T>) {
        unsafe {
            ffi::cppgc_visitor_trace(&raw mut self.visitor, handle.as_ffi_ref());
        }
    }

    /// Traces a strong member reference.
    pub fn trace_member(&mut self, member: &cppgc::Member) {
        if let Some(ref m) = member.handle {
            unsafe {
                ffi::cppgc_visitor_trace_member(&raw mut self.visitor, m.as_ref());
            }
        }
    }

    /// Traces a weak member reference.
    ///
    /// This informs the garbage collector about a weak reference. If the target
    /// is collected, the weak member will be automatically cleared.
    pub fn trace_weak_member(&mut self, member: &cppgc::WeakMember) {
        if let Some(ref m) = member.handle {
            unsafe {
                ffi::cppgc_visitor_trace_weak_member(&raw mut self.visitor, m.as_ref());
            }
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
/// let isolate = unsafe { v8::Isolate::from_raw(raw_ptr) };
///
/// // Check if locked before V8 operations
/// assert!(isolate.is_locked());
///
/// // Get raw pointer for FFI calls
/// let ptr = isolate.as_ptr();
/// ```
#[derive(Clone, Copy, Debug)]
pub struct Isolate {
    handle: NonNull<ffi::Isolate>,
}

impl Isolate {
    /// Creates an `Isolate` from a raw pointer.
    ///
    /// # Safety
    /// The pointer must be non-null and point to a valid V8 isolate.
    pub unsafe fn from_raw(handle: *mut ffi::Isolate) -> Self {
        debug_assert!(unsafe { ffi::isolate_is_locked(handle) });
        Self {
            handle: unsafe { NonNull::new_unchecked(handle) },
        }
    }

    /// Creates an `Isolate` from a `NonNull` pointer.
    pub fn from_non_null(handle: NonNull<ffi::Isolate>) -> Self {
        debug_assert!(unsafe { ffi::isolate_is_locked(handle.as_ptr()) });
        Self { handle }
    }

    /// Returns whether this isolate is currently locked by the current thread.
    pub fn is_locked(&self) -> bool {
        unsafe { ffi::isolate_is_locked(self.handle.as_ptr()) }
    }

    /// Returns the raw pointer to the V8 isolate.
    pub fn as_ptr(&self) -> *mut ffi::Isolate {
        self.handle.as_ptr()
    }

    /// Returns the `NonNull` pointer to the V8 isolate.
    pub fn as_non_null(&self) -> NonNull<ffi::Isolate> {
        self.handle
    }
}
