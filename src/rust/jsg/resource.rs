use std::any::Any;
use std::any::TypeId;
use std::cell::Cell;
use std::cell::UnsafeCell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::ops::Deref;
use std::ptr::NonNull;

use crate::Lock;
use crate::Realm;
use crate::Resource;
use crate::ResourceTemplate;
use crate::v8;

// ============================================================================
// ResourceCleanup trait for type-erased cleanup
// ============================================================================

/// Trait for type-erased resource cleanup during Realm shutdown.
///
/// This follows the C++ pattern where `HeapTracer::clearWrappers()` iterates all tracked
/// wrappers at shutdown and cleans them up. Without this, instances that have JavaScript
/// wrappers but are waiting for V8 GC would leak when the Realm is dropped.
pub(crate) trait ResourceCleanup: Any {
    /// Cleans up all tracked instances by dropping them directly.
    /// Called during Realm shutdown before V8 GC has a chance to run.
    fn cleanup(&mut self);

    /// Removes an instance from tracking. Called from the weak callback when V8 GC
    /// collects a wrapped resource. This prevents double-free when Realm later drops.
    fn remove_instance(&mut self, ptr: NonNull<c_void>);

    /// Returns self as `&mut dyn Any` for downcasting.
    fn as_any_mut(&mut self) -> &mut dyn Any;
}

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
// WrapperInfo - Data associated with a JavaScript-wrapped instance
// ============================================================================

/// Information stored when an instance is exposed to JavaScript.
///
/// All fields are set atomically when `attach_wrapper` is called, ensuring
/// we never have a partially-initialized wrapper state.
struct WrapperInfo {
    /// Pointer to the owning Instance (for the weak callback).
    this: NonNull<c_void>,
    /// V8 Global handle to the JavaScript wrapper object.
    wrapper: v8::Global<v8::Object>,
    /// The V8 isolate this instance is bound to.
    isolate: NonNull<v8::ffi::Isolate>,
    /// `TypeId` of the resource type R (for finding `ResourceImpl` during cleanup).
    type_id: TypeId,
}

// ============================================================================
// State - Lifecycle state for a resource instance
// ============================================================================

/// Tracks the lifecycle state and reference count for a resource instance.
///
/// # Lifecycle
///
/// 1. **Created**: Instance allocated with `ref_count` = 0, then `Ref::new` increments to 1
/// 2. **Wrapped**: `attach_wrapper` called, wrapper = Some(WrapperInfo)
/// 3. **Ref count zero with wrapper**: Global made weak, waiting for GC
/// 4. **Dropped**: Either via weak callback (GC) or Realm cleanup
///
/// # Interior Mutability
///
/// `ref_count` uses `Cell` for interior mutability since it's modified through `&self`
/// during `Clone`. The `wrapper` field is only modified through `&mut self`.
pub struct State {
    /// Reference count. Modified through `&self` in Clone.
    ref_count: Cell<usize>,
    /// Function pointer to drop the Instance. Required for type-erased dropping
    /// from the V8 weak callback.
    drop_fn: unsafe fn(*mut c_void),
    /// Wrapper information, set when exposed to JavaScript.
    info: Option<WrapperInfo>,
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
            info: None,
        }
    }

    /// Returns the current reference count.
    pub(crate) fn ref_count(&self) -> usize {
        self.ref_count.get()
    }

    /// Increments the reference count and returns the new value.
    pub(crate) fn inc_ref_count(&self) -> usize {
        let count = self.ref_count.get();
        self.ref_count.set(count + 1);
        count + 1
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

    /// Returns whether this instance has been wrapped for JavaScript.
    pub(crate) fn is_wrapped(&self) -> bool {
        self.info.is_some()
    }

    /// Returns the V8 Global wrapper handle if wrapped.
    pub(crate) fn wrapper(&self) -> Option<&v8::Global<v8::Object>> {
        self.info.as_ref().map(|w| &w.wrapper)
    }

    /// Returns the isolate pointer if wrapped.
    pub fn isolate(&self) -> Option<NonNull<v8::ffi::Isolate>> {
        self.info.as_ref().map(|w| w.isolate)
    }

    /// Returns the `TypeId` if wrapped.
    pub fn type_id(&self) -> Option<TypeId> {
        self.info.as_ref().map(|w| w.type_id)
    }

    /// Returns the instance pointer if wrapped.
    pub fn this_ptr(&self) -> Option<NonNull<c_void>> {
        self.info.as_ref().map(|w| w.this)
    }

    /// Returns the drop function.
    pub fn drop_fn(&self) -> unsafe fn(*mut c_void) {
        self.drop_fn
    }

    /// Makes the V8 Global handle weak, allowing GC to collect it.
    ///
    /// # Safety
    /// The instance must still be valid and the isolate must be correct.
    pub(crate) unsafe fn make_weak(&mut self) {
        // Get state_ptr before borrowing wrapper to avoid double mutable borrow
        let state_ptr = std::ptr::from_mut(self).cast::<c_void>();
        if let Some(ref mut info) = self.info {
            unsafe { info.wrapper.make_weak(info.isolate.as_ptr(), state_ptr) };
        }
    }

    /// Attaches a V8 wrapper object to this resource state.
    ///
    /// # Panics
    /// Panics if already wrapped.
    pub fn attach_wrapper<R: Resource + 'static>(
        &mut self,
        realm: &mut Realm,
        object: v8::Local<v8::Object>,
        this_ptr: NonNull<c_void>,
    ) where
        <R as Resource>::Template: 'static,
    {
        debug_assert!(
            self.info.is_none(),
            "attach_wrapper called on already-wrapped instance"
        );

        self.info = Some(WrapperInfo {
            this: this_ptr,
            wrapper: object.into(),
            isolate: realm.isolate(),
            type_id: TypeId::of::<R>(),
        });

        realm.get_resources::<R>().add_instance(this_ptr);
    }
}

// ============================================================================
// Instance - Container for a resource and its state
// ============================================================================

/// Container holding a resource value and its lifecycle state.
///
/// Uses `UnsafeCell` for interior mutability, following Rust conventions
/// for types that need mutable access through shared references.
pub(crate) struct Instance<R: Resource> {
    /// The actual resource value.
    resource: UnsafeCell<R>,
    /// Lifecycle state (ref count, wrapper info).
    state: UnsafeCell<State>,
}

impl<R: Resource> Instance<R> {
    /// Creates a new instance and returns a reference-counted handle to it.
    #[expect(clippy::new_ret_no_self)]
    pub fn new(resource: R) -> Ref<R> {
        let instance = Box::new(Self {
            resource: UnsafeCell::new(resource),
            state: UnsafeCell::new(State::new(Self::drop_instance)),
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

    /// Returns a reference to the state.
    ///
    /// # Safety
    /// No mutable references to state may exist.
    unsafe fn state(&self) -> &State {
        unsafe { &*self.state.get() }
    }

    /// Returns a mutable reference to the state.
    ///
    /// # Safety
    /// No other references to state may exist.
    #[expect(clippy::mut_from_ref)]
    unsafe fn state_mut(&self) -> &mut State {
        unsafe { &mut *self.state.get() }
    }

    /// Returns a reference to the resource.
    ///
    /// # Safety
    /// No mutable references to resource may exist.
    unsafe fn resource(&self) -> &R {
        unsafe { &*self.resource.get() }
    }

    /// Returns a mutable reference to the resource.
    ///
    /// # Safety
    /// No other references to resource may exist.
    #[expect(clippy::mut_from_ref)]
    unsafe fn resource_mut(&self) -> &mut R {
        unsafe { &mut *self.resource.get() }
    }

    /// Handles reference count reaching zero.
    ///
    /// If wrapped, makes the Global weak so GC can collect it.
    /// If not wrapped, drops the instance immediately.
    pub(crate) fn handle_ref_count_zero(ptr: InstancePtr<R>) {
        // SAFETY: ptr is valid, we're the only accessor at this point
        let state = unsafe { (*ptr.as_ptr()).state_mut() };

        if state.is_wrapped() {
            // Make the V8 Global weak - GC will trigger drop via weak callback
            // SAFETY: Instance is still valid, isolate is correct
            unsafe { state.make_weak() };
        } else {
            // No JavaScript wrapper, drop immediately
            // SAFETY: No wrapper means no other references, safe to drop
            unsafe { ptr.drop_in_place() };
        }
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
        let state = unsafe { (*ptr.as_ptr()).state() };
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
        unsafe { (*self.ptr.as_ptr()).resource() }
    }
}

impl<R: Resource> Clone for Ref<R> {
    fn clone(&self) -> Self {
        // SAFETY: ptr is valid while Ref exists
        let state = unsafe { (*self.ptr.as_ptr()).state() };
        state.inc_ref_count();
        Self { ptr: self.ptr }
    }
}

impl<R: Resource> Drop for Ref<R> {
    fn drop(&mut self) {
        // SAFETY: ptr is valid while Ref exists
        let state = unsafe { (*self.ptr.as_ptr()).state() };
        let new_count = state.dec_ref_count();

        if new_count == 0 {
            Instance::handle_ref_count_zero(self.ptr);
        }
    }
}

// ============================================================================
// wrap/unwrap - JavaScript interop
// ============================================================================

/// Wraps a Rust resource for exposure to JavaScript.
///
/// If the resource is already wrapped, returns the existing wrapper.
/// Otherwise creates a new V8 object wrapper.
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
    // SAFETY: ptr is valid because Ref maintains the invariant
    let state = unsafe { (*ptr.as_ptr()).state_mut() };

    if let Some(wrapper) = state.wrapper() {
        let local = wrapper.as_local(lock);
        if local.has_value() {
            return local.into();
        }
    }

    let resources = lock.realm().get_resources::<R>();
    let constructor = resources.get_constructor(lock);

    let this_ptr = ptr.as_erased();

    // SAFETY: V8 FFI call with valid isolate and constructor
    let wrapped: v8::Local<'a, v8::Value> = unsafe {
        v8::Local::from_ffi(
            lock.isolate().as_ptr(),
            v8::ffi::wrap_resource(
                lock.isolate().as_ptr(),
                this_ptr.as_ptr() as usize,
                constructor.as_ffi_ref(),
            ),
        )
    };

    state.attach_wrapper::<R>(lock.realm(), wrapped.clone().into(), this_ptr);

    wrapped
}

/// Unwraps a JavaScript value to get a reference to the underlying Rust resource.
///
/// # Safety
/// - The `value` must be a valid JavaScript wrapper for type `R`.
/// - No other references (mutable or immutable) to the same resource instance
///   may exist for the duration of the returned borrow. This includes references
///   obtained via `Deref` on `Ref<R>` handles pointing to the same instance.
pub fn unwrap<'a, R: Resource>(lock: &'a mut Lock, value: v8::Local<v8::Value>) -> &'a mut R {
    // SAFETY: Caller guarantees value wraps an Instance<R>
    let ptr = unsafe {
        v8::ffi::unwrap_resource(lock.isolate().as_ptr(), value.into_ffi()) as *mut Instance<R>
    };
    // SAFETY: Caller guarantees no other references to this resource exist.
    // This is documented in the function's safety requirements.
    unsafe { (*ptr).resource_mut() }
}

// ============================================================================
// Resources - Per-realm resource tracking
// ============================================================================

/// Stores per-type resource templates and instances, keyed by `TypeId`.
///
/// Each entry maps a Rust resource type to its `ResourceImpl<R>` which tracks
/// the V8 function template and all live instances of that type.
#[derive(Default)]
pub struct Resources {
    templates: HashMap<TypeId, Box<dyn ResourceCleanup>>,
}

impl Drop for Resources {
    fn drop(&mut self) {
        // Clean up all tracked instances during Realm shutdown.
        // This follows the C++ HeapTracer::clearWrappers() pattern.
        for resource_impl in self.templates.values_mut() {
            resource_impl.cleanup();
        }
    }
}

impl Resources {
    /// Gets or creates the resource tracking structure for a given resource type.
    ///
    /// # Panics
    /// Panics if type mismatch (indicates a bug).
    pub fn get_or_create<R: Resource + 'static>(&mut self, _lock: &mut Lock) -> &mut ResourceImpl<R>
    where
        <R as Resource>::Template: 'static,
    {
        self.templates
            .entry(TypeId::of::<R>())
            .or_insert_with(|| Box::new(ResourceImpl::<R>::default()))
            .as_any_mut()
            .downcast_mut::<ResourceImpl<R>>()
            .expect("Template type mismatch")
    }

    /// Removes an instance from tracking by its `TypeId`.
    ///
    /// Called from the weak callback when V8 GC collects a wrapped resource.
    pub(crate) fn remove_instance_by_type_id(&mut self, type_id: TypeId, ptr: NonNull<c_void>) {
        if let Some(resource_impl) = self.templates.get_mut(&type_id) {
            resource_impl.remove_instance(ptr);
        }
    }
}

// ============================================================================
// ResourceImpl - Per-type resource tracking
// ============================================================================

/// Tracks the V8 function template and all live instances for a single resource type.
///
/// The template is lazily initialized on first use. Instances are tracked so they
/// can be cleaned up when the Realm is dropped.
pub struct ResourceImpl<R: Resource> {
    template: Option<R::Template>,
    /// Type-erased instance pointers for cleanup tracking.
    /// Uses `NonNull<c_void>` because `ResourceCleanup` trait is not generic.
    instances: Vec<NonNull<c_void>>,
    /// `PhantomData` to tie the type parameter
    _marker: std::marker::PhantomData<R>,
}

impl<R: Resource> Default for ResourceImpl<R> {
    fn default() -> Self {
        Self {
            template: None,
            instances: Vec::new(),
            _marker: std::marker::PhantomData,
        }
    }
}

impl<R: Resource + 'static> ResourceCleanup for ResourceImpl<R>
where
    <R as Resource>::Template: 'static,
{
    fn cleanup(&mut self) {
        // Drop all tracked instances directly.
        // This is called during Realm shutdown.
        for erased_ptr in self.instances.drain(..) {
            // SAFETY: ptr was created from InstancePtr<R>, instance is still valid
            let ptr = unsafe { InstancePtr::<R>::from_erased(erased_ptr) };
            unsafe { ptr.drop_in_place() };
        }
    }

    fn remove_instance(&mut self, ptr: NonNull<c_void>) {
        // TODO(soon): This O(n) approach is going to get slower and slower with large instances list.
        // Remove from tracking (called from weak callback before dropping)
        if let Some(pos) = self.instances.iter().position(|p| *p == ptr) {
            self.instances.swap_remove(pos);
        }
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

impl<R: Resource> ResourceImpl<R> {
    /// Adds an instance to tracking.
    pub(crate) fn add_instance(&mut self, ptr: NonNull<c_void>) {
        self.instances.push(ptr);
    }

    /// Returns the V8 function template constructor for this resource type.
    ///
    /// Lazily creates the template on first access.
    pub fn get_constructor(&mut self, lock: &mut Lock) -> &v8::Global<v8::FunctionTemplate> {
        self.template
            .get_or_insert_with(|| R::Template::new(lock))
            .get_constructor()
    }
}
