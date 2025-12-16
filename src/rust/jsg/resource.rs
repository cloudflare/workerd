//! Resource management for Rust types exposed to JavaScript.
//!
//! This module provides the infrastructure for exposing Rust resources to JavaScript
//! with proper lifetime management through V8's cppgc garbage collector.
//!
//! # Key Types
//!
//! - [`Ref<R>`] - Reference-counted handle to a resource, analogous to `jsg::Ref<T>` in C++
//! - [`WeakRef<R>`] - Weak reference that doesn't prevent garbage collection
//! - [`State`] - Internal lifecycle state tracking for resource instances
//!
//! # Lifecycle
//!
//! 1. Resource created via `Resource::alloc()`, returning a `Ref<R>`
//! 2. When wrapped for JavaScript, a cppgc `RustResource` is allocated
//! 3. While `Ref<R>` handles exist, cppgc persistent keeps resource alive
//! 4. When last `Ref<R>` drops, persistent is released for GC
//! 5. cppgc collects the resource, invoking the Rust destructor

use std::any::TypeId;
use std::cell::Cell;
use std::ffi::c_void;
use std::ops::Deref;
use std::ptr::NonNull;

use crate::GarbageCollected;
use crate::Lock;
use crate::Realm;
use crate::Resource;
use crate::ResourceTemplate;
use crate::v8;

// ============================================================================
// InstancePtr - Type-safe wrapper for heap-allocated Instance pointers
// ============================================================================

/// Type-safe handle to a heap-allocated `Instance<R>`.
///
/// This wrapper preserves type information and provides a safer interface than raw pointers.
/// The instance is heap-allocated via `Box::into_raw` and must be dropped via `drop_in_place`.
#[repr(transparent)]
pub(crate) struct InstancePtr<R: Resource>(NonNull<Instance<R>>);

// Manual Clone/Copy impls to avoid requiring R: Clone + Copy
impl<R: Resource> Clone for InstancePtr<R> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<R: Resource> Copy for InstancePtr<R> {}

impl<R: Resource> InstancePtr<R> {
    /// Creates a new `InstancePtr` from a boxed instance.
    ///
    /// Takes ownership of the instance and leaks it onto the heap.
    /// The caller is responsible for eventually calling `drop_in_place`.
    fn new(instance: Box<Instance<R>>) -> Self {
        // SAFETY: Box::into_raw never returns null
        Self(unsafe { NonNull::new_unchecked(Box::into_raw(instance)) })
    }

    /// Returns the raw pointer for FFI purposes.
    fn as_ptr(self) -> *mut Instance<R> {
        self.0.as_ptr()
    }

    /// Returns a type-erased pointer for storage in collections.
    fn as_erased(self) -> NonNull<c_void> {
        self.0.cast()
    }

    /// Creates an `InstancePtr` from a type-erased pointer.
    ///
    /// # Safety
    /// The pointer must have been created from an `InstancePtr<R>` of the same type.
    unsafe fn from_erased(ptr: NonNull<c_void>) -> Self {
        Self(ptr.cast())
    }

    /// Consumes this handle and drops the instance.
    ///
    /// # Safety
    /// - Must only be called once per instance
    /// - The instance must still be valid (not already dropped)
    unsafe fn drop_in_place(self) {
        let _ = unsafe { Box::from_raw(self.0.as_ptr()) };
    }
}

// ============================================================================
// WrapperState - Lifecycle state for JavaScript wrapper
// ============================================================================

/// Wrapper state for a resource instance.
///
/// Represents whether an instance has been exposed to JavaScript and,
/// if so, the associated wrapper information. Uses `TracedReference` (cppgc/Oilpan)
/// for proper GC integration.
enum WrapperState {
    /// Instance has not been exposed to JavaScript.
    NotWrapped,
    /// Instance has a JavaScript wrapper.
    ///
    /// All fields are set atomically when `attach_wrapper` is called, ensuring
    /// we never have a partially-initialized wrapper state.
    Wrapped {
        /// Pointer to the owning Instance (for cleanup).
        this: NonNull<c_void>,
        /// `TracedReference` to the JavaScript wrapper object (cppgc-aware).
        wrapper: v8::TracedReference<v8::Object>,
        /// The V8 isolate this instance is bound to.
        isolate: v8::Isolate,
        /// `TypeId` of the resource type R (for finding `ResourceImpl` during cleanup).
        type_id: TypeId,
    },
}

// ============================================================================
// State - Lifecycle state for a resource instance
// ============================================================================

/// Tracks the lifecycle state and reference count for a resource instance.
///
/// # Lifecycle
///
/// 1. **Created**: Instance allocated with `ref_count` = 0, then `Ref::new` increments to 1
/// 2. **Wrapped**: `attach_wrapper` called, state becomes `Wrapped { ... }`
/// 3. **Ref count zero**: `CppgcHandle` is released, allowing cppgc to GC
/// 4. **Dropped**: Either via cppgc GC or Realm cleanup
///
/// # Interior Mutability
///
/// `ref_count` uses `Cell` for interior mutability since it's modified through `&self`
/// during `Clone`. The `wrapper` field is only modified through `&mut self`.
pub struct State {
    /// Reference count. Modified through `&self` in Clone.
    ref_count: Cell<usize>,
    /// Function pointer to drop the Instance. Required for type-erased dropping
    /// from cppgc GC.
    drop_fn: unsafe fn(*mut c_void),
    /// Wrapper state, tracks whether exposed to JavaScript.
    wrapper: WrapperState,
    /// cppgc handle that keeps the `RustResource` alive while Rust has references.
    /// When released, cppgc can garbage collect the resource.
    cppgc_handle: v8::cppgc::Handle,
}

impl State {
    /// Creates a new state with the given drop function.
    ///
    /// The drop function must correctly drop an `Instance<R>` when given
    /// a pointer to it. This is set up by `Instance::new`.
    pub fn new(drop_fn: unsafe fn(*mut c_void)) -> Self {
        Self {
            ref_count: Cell::new(0),
            drop_fn,
            wrapper: WrapperState::NotWrapped,
            cppgc_handle: v8::cppgc::Handle::new(),
        }
    }

    /// Returns a reference to the cppgc handle.
    pub fn cppgc_handle(&self) -> &v8::cppgc::Handle {
        &self.cppgc_handle
    }

    /// Returns a mutable reference to the cppgc handle.
    pub fn cppgc_handle_mut(&mut self) -> &mut v8::cppgc::Handle {
        &mut self.cppgc_handle
    }

    /// Returns the current reference count.
    pub(crate) fn ref_count(&self) -> usize {
        self.ref_count.get()
    }

    /// Increments the reference count.
    pub(crate) fn inc_ref_count(&self) {
        let count = self.ref_count.get();
        self.ref_count.set(count + 1);
    }

    /// Decrements the reference count and returns the new value.
    ///
    /// # Panics
    /// Debug-panics if called when count is already 0.
    pub(crate) fn dec_ref_count(&self) -> usize {
        let count = self.ref_count.get();
        debug_assert!(count > 0, "dec_ref_count called when count is already 0");
        self.ref_count.set(count - 1);
        count - 1
    }

    /// Returns the `TracedReference` wrapper handle if wrapped.
    pub(crate) fn traced_reference(&self) -> Option<&v8::TracedReference<v8::Object>> {
        match &self.wrapper {
            WrapperState::NotWrapped => None,
            WrapperState::Wrapped { wrapper, .. } => Some(wrapper),
        }
    }

    /// Returns the isolate if wrapped.
    pub fn isolate(&self) -> Option<v8::Isolate> {
        match &self.wrapper {
            WrapperState::NotWrapped => None,
            WrapperState::Wrapped { isolate, .. } => Some(*isolate),
        }
    }

    /// Returns the `TypeId` if wrapped.
    pub fn type_id(&self) -> Option<TypeId> {
        match &self.wrapper {
            WrapperState::NotWrapped => None,
            WrapperState::Wrapped { type_id, .. } => Some(*type_id),
        }
    }

    /// Returns the instance pointer if wrapped.
    pub fn this_ptr(&self) -> Option<NonNull<c_void>> {
        match &self.wrapper {
            WrapperState::NotWrapped => None,
            WrapperState::Wrapped { this, .. } => Some(*this),
        }
    }

    /// Returns the drop function.
    pub fn drop_fn(&self) -> unsafe fn(*mut c_void) {
        self.drop_fn
    }

    /// Attaches a V8 wrapper object to this resource state.
    ///
    /// # Panics
    /// Panics if already wrapped.
    pub fn set_wrapper<R: Resource + 'static>(
        &mut self,
        realm: &Realm,
        object: v8::Local<v8::Object>,
        this_ptr: NonNull<c_void>,
    ) {
        debug_assert!(
            matches!(self.wrapper, WrapperState::NotWrapped),
            "attach_wrapper called on already-wrapped instance"
        );

        self.wrapper = WrapperState::Wrapped {
            this: this_ptr,
            wrapper: object.into(),
            isolate: realm.isolate(),
            type_id: TypeId::of::<R>(),
        };
    }
}

// ============================================================================
// Instance - Container for a resource and its state
// ============================================================================

/// Container holding a resource value and its lifecycle state.
pub(crate) struct Instance<R: Resource> {
    /// The actual resource value.
    pub resource: R,
    /// Lifecycle state (ref count, wrapper info).
    pub state: State,
}

impl<R: Resource> Instance<R> {
    /// Creates a new instance and returns a reference-counted handle to it.
    #[expect(clippy::new_ret_no_self)]
    pub fn new(resource: R) -> Ref<R> {
        let instance = Box::new(Self {
            resource,
            state: State::new(Self::drop_instance),
        });
        Ref::new(InstancePtr::new(instance))
    }

    /// Static function that drops an Instance<R> given a type-erased pointer.
    ///
    /// This is stored in State and called from the weak callback or cleanup.
    ///
    /// # Safety
    /// `ptr` must point to a valid, heap-allocated `Instance<R>`.
    unsafe fn drop_instance(ptr: *mut c_void) {
        debug_assert!(!ptr.is_null(), "drop_instance called with null pointer");
        let _ = unsafe { Box::from_raw(ptr.cast::<Self>()) };
    }
}

// ============================================================================
// Ref - Reference-counted handle to a resource
// ============================================================================

/// A reference-counted handle to a Rust resource.
///
/// Analogous to `jsg::Ref<T>` in C++ JSG. When all `Ref` handles are dropped
/// and a JavaScript wrapper exists, the V8 Global becomes weak, allowing GC
/// to reclaim the resource.
///
/// # Thread Safety
///
/// `Ref<T>` is **not thread-safe**. Resources are bound to the V8 isolate's
/// thread and must not be sent across threads.
///
/// # Invariants
///
/// - `ptr` always points to a valid, heap-allocated `Instance<R>`
/// - Reference count is always >= 1 while any `Ref` exists
pub struct Ref<R: Resource> {
    ptr: InstancePtr<R>,
}

impl<R: Resource> Ref<R> {
    /// Creates a new `Ref` from an instance pointer.
    ///
    /// Initializes the reference count to 1.
    pub(crate) fn new(ptr: InstancePtr<R>) -> Self {
        // SAFETY: ptr is valid, just created
        let state = unsafe { &mut (*ptr.as_ptr()).state };
        debug_assert_eq!(state.ref_count(), 0);
        state.inc_ref_count();
        Self { ptr }
    }

    /// Returns the instance pointer for internal use.
    pub(crate) fn instance_ptr(&self) -> InstancePtr<R> {
        self.ptr
    }
}

impl<R: Resource> Deref for Ref<R> {
    type Target = R;

    fn deref(&self) -> &Self::Target {
        // SAFETY: Ref maintains invariant that ptr is valid
        unsafe { &(*self.ptr.as_ptr()).resource }
    }
}

impl<R: Resource> Clone for Ref<R> {
    fn clone(&self) -> Self {
        // SAFETY: ptr is valid while Ref exists
        let state = unsafe { &(*self.ptr.as_ptr()).state };
        debug_assert!(state.ref_count() >= 1, "Clone called with ref_count < 1");
        state.inc_ref_count();
        Self { ptr: self.ptr }
    }
}

impl<R: Resource> Drop for Ref<R> {
    fn drop(&mut self) {
        // SAFETY: ptr is valid while Ref exists
        let state = unsafe { &(*self.ptr.as_ptr()).state };
        let new_count = state.dec_ref_count();

        if new_count == 0 {
            // SAFETY: ref_count is 0, we're the only accessor
            let state = unsafe { &mut (*self.ptr.as_ptr()).state };

            if state.cppgc_handle.has_persistent() {
                // Release the cppgc handle - GC will trigger drop via RustResource destructor
                state.cppgc_handle.release();
            } else {
                // No cppgc allocation, drop immediately
                // SAFETY: No cppgc handle means we own the instance, safe to drop
                unsafe { self.ptr.drop_in_place() };
            }
        }
    }
}

// ============================================================================
// WeakRef - Weak reference to a resource
// ============================================================================

/// A weak reference to a Rust resource.
///
/// Unlike `Ref<R>`, a `WeakRef` does not prevent the resource from being
/// garbage collected. Use `upgrade()` to attempt to get a strong `Ref<R>`.
///
/// `WeakRef` holds a `CppgcWeakMember` for proper GC integration. When traced,
/// the garbage collector is informed of the weak relationship, and the weak
/// member will be automatically cleared when the target is collected.
pub struct WeakRef<R: Resource> {
    ptr: InstancePtr<R>,
    /// Weak member for cppgc tracing. Only set if the target resource has been wrapped.
    weak_member: v8::cppgc::WeakMember,
}

impl<R: Resource> WeakRef<R> {
    /// Attempts to upgrade this weak reference to a strong `Ref<R>`.
    ///
    /// Returns `Some(Ref<R>)` if the resource is still alive (cppgc weak member
    /// is alive and `ref_count` > 0), or `None` otherwise.
    pub fn upgrade(&self) -> Option<Ref<R>> {
        // First check if cppgc has collected the target
        if self.weak_member.get().is_none() && self.has_cppgc_member() {
            return None;
        }
        // SAFETY: ptr is valid while any Ref or JS wrapper exists
        let state = unsafe { &(*self.ptr.as_ptr()).state };
        if state.ref_count() > 0 {
            state.inc_ref_count();
            Some(Ref { ptr: self.ptr })
        } else {
            None
        }
    }

    /// Returns the number of strong references.
    pub fn strong_count(&self) -> usize {
        // SAFETY: ptr is valid while any Ref or JS wrapper exists
        let state = unsafe { &(*self.ptr.as_ptr()).state };
        state.ref_count()
    }

    /// Returns whether this weak ref has a cppgc member set.
    fn has_cppgc_member(&self) -> bool {
        self.weak_member.is_alive() || self.weak_member.get().is_some()
    }
}

impl<R: Resource> GarbageCollected for WeakRef<R> {
    /// Traces this weak reference for garbage collection.
    ///
    /// This should be called during the owning resource's trace method
    /// to inform the GC about the weak relationship.
    fn trace(&self, visitor: &mut v8::GcVisitor) {
        visitor.trace_weak_member(&self.weak_member);
    }
}

impl<R: Resource> From<&Ref<R>> for WeakRef<R> {
    fn from(r: &Ref<R>) -> Self {
        // SAFETY: Ref guarantees ptr is valid while the Ref exists
        let state = unsafe { &(*r.ptr.as_ptr()).state };
        let weak_member = state
            .cppgc_handle()
            .get_resource()
            .map(v8::cppgc::WeakMember::from_resource)
            .unwrap_or_default();

        Self {
            ptr: r.ptr,
            weak_member,
        }
    }
}

impl<R: Resource> Clone for WeakRef<R> {
    fn clone(&self) -> Self {
        // SAFETY: WeakRef is only created from valid Ref, and ptr remains valid
        // while any Ref or JS wrapper exists
        let state = unsafe { &(*self.ptr.as_ptr()).state };
        let weak_member = state
            .cppgc_handle()
            .get_resource()
            .map(v8::cppgc::WeakMember::from_resource)
            .unwrap_or_default();

        Self {
            ptr: self.ptr,
            weak_member,
        }
    }
}

// ============================================================================
// wrap/unwrap - JavaScript interop
// ============================================================================

/// Generic trace function for resources.
///
/// # Safety
/// `ptr` must point to a valid Instance<R>.
unsafe fn resource_trace<R: Resource>(ptr: *mut c_void, visitor: *mut v8::ffi::CppgcVisitor) {
    if ptr.is_null() || visitor.is_null() {
        return;
    }
    let instance = unsafe { &*(ptr as *const Instance<R>) };
    let mut gc_visitor = unsafe {
        v8::GcVisitor::from_raw(v8::ffi::CppgcVisitor {
            ptr: visitor as usize,
        })
    };
    instance.resource.trace(&mut gc_visitor);
}

/// Wraps a Rust resource for exposure to JavaScript.
///
/// If the resource is already wrapped, returns the existing wrapper.
/// Otherwise creates a new V8 object wrapper and allocates on cppgc heap.
///
/// # Panics
/// Panics if internal state is inconsistent (indicates a bug).
#[expect(clippy::needless_pass_by_value)]
pub fn wrap<'a, R: Resource + 'static>(
    lock: &mut Lock,
    resource: Ref<R>,
) -> v8::Local<'a, v8::Value>
where
    <R as Resource>::Template: 'static,
{
    let ptr = resource.instance_ptr();
    // SAFETY: Ref guarantees ptr is valid
    let state = unsafe { &mut (*ptr.as_ptr()).state };

    if let Some(traced) = state.traced_reference() {
        let local = traced.get(lock);
        if local.has_value() {
            return local.into();
        }
    }

    let resources = lock.realm().get_resources::<R>();
    let constructor = resources.get_constructor(lock);
    let this_ptr = ptr.as_erased();

    if !state.cppgc_handle.has_persistent() {
        let data = v8::ffi::RustResourceData {
            instance_ptr: this_ptr.as_ptr() as usize,
            drop_fn: state.drop_fn() as usize,
            trace_fn: resource_trace::<R> as usize,
        };
        // SAFETY: data contains valid function pointers from Instance<R>
        let rust_resource = unsafe { lock.alloc(data) };
        // SAFETY: rust_resource was just allocated on cppgc heap
        *state.cppgc_handle_mut() = unsafe { v8::cppgc::Handle::from_resource(rust_resource) };
    }

    // SAFETY: Lock guarantees isolate is valid; constructor is a valid Global handle
    let wrapped: v8::Local<'a, v8::Value> = unsafe {
        v8::Local::from_ffi(
            lock.isolate(),
            v8::ffi::wrap_resource(
                lock.isolate().as_ptr(),
                this_ptr.as_ptr() as usize,
                constructor.as_ffi_ref(),
            ),
        )
    };

    state.set_wrapper::<R>(lock.realm(), wrapped.clone().into(), this_ptr);

    wrapped
}

/// Unwraps a JavaScript value to get a reference to the underlying Rust resource.
///
/// # Safety
/// - The `value` must be a valid JavaScript wrapper for type `R`.
/// - No other references (mutable or immutable) to the same resource instance
///   may exist for the duration of the returned borrow. This includes references
///   obtained via `Deref` on `Ref<R>` handles pointing to the same instance.
pub unsafe fn unwrap<'a, R: Resource>(
    lock: &'a mut Lock,
    value: v8::Local<v8::Value>,
) -> &'a mut R {
    // SAFETY: Caller guarantees value wraps an Instance<R>
    let ptr = unsafe {
        v8::ffi::unwrap_resource(lock.isolate().as_ptr(), value.into_ffi()) as *mut Instance<R>
    };
    // SAFETY: Caller guarantees no other references to this resource exist
    unsafe { &mut (*ptr).resource }
}

/// Unwraps a JavaScript value to get a reference-counted handle to the resource.
///
/// This creates a new `Ref<R>` handle, incrementing the reference count.
/// If the wrapper was weak (no Rust Refs existed), it upgrades to strong.
///
/// # Safety
/// - The `value` must be a valid JavaScript wrapper for type `R`.
pub unsafe fn unwrap_ref<R: Resource>(lock: &mut Lock, value: v8::Local<v8::Value>) -> Ref<R> {
    // SAFETY: Caller guarantees value wraps an Instance<R>
    let ptr = unsafe {
        v8::ffi::unwrap_resource(lock.isolate().as_ptr(), value.into_ffi()) as *mut Instance<R>
    };
    let ptr = unsafe { InstancePtr::from_erased(NonNull::new_unchecked(ptr.cast())) };

    // SAFETY: ptr is valid from the unwrap
    let state = unsafe { &mut (*ptr.as_ptr()).state };
    state.inc_ref_count();
    Ref { ptr }
}

// ============================================================================
// ResourceImpl - Per-type resource tracking
// ============================================================================

/// Tracks the V8 function template for a single resource type.
///
/// The template is lazily initialized on first use. Instance cleanup is handled
/// by cppgc when the isolate is destroyed.
pub struct ResourceImpl<R: Resource> {
    template: Option<R::Template>,
    _marker: std::marker::PhantomData<R>,
}

impl<R: Resource> Default for ResourceImpl<R> {
    fn default() -> Self {
        Self {
            template: None,
            _marker: std::marker::PhantomData,
        }
    }
}

impl<R: Resource> ResourceImpl<R> {
    /// Returns the V8 function template constructor for this resource type.
    ///
    /// Lazily creates the template on first access.
    pub fn get_constructor(&mut self, lock: &mut Lock) -> &v8::Global<v8::FunctionTemplate> {
        self.template
            .get_or_insert_with(|| R::Template::new(lock))
            .get_constructor()
    }
}
