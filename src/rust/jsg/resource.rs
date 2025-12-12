use std::any::Any;
use std::any::TypeId;
use std::cell::Cell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::ops::Deref;
use std::ops::DerefMut;
use std::ptr::NonNull;

use crate::Lock;
use crate::Realm;
use crate::Resource;
use crate::ResourceTemplate;
use crate::v8;

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

pub(crate) struct Instance<R: Resource> {
    resource: R,
    pub(crate) state: State,
}

impl<R: Resource> Instance<R> {
    #[expect(clippy::new_ret_no_self)]
    pub fn new(resource: R) -> Ref<R> {
        let instance = Self {
            resource,
            state: State::new(Self::drop_instance),
        };
        Ref::new(instance)
    }

    /// Static function that can be stored as a function pointer to drop this Instance type.
    ///
    /// # Safety
    /// The `ptr` must point to a valid, heap-allocated `Instance<R>` that was created via
    /// `Box::into_raw` in `Ref::new`. This function takes ownership and deallocates the memory.
    unsafe fn drop_instance(ptr: *mut c_void) {
        debug_assert!(!ptr.is_null(), "drop_instance called with null pointer");
        // Reconstruct the Box to run destructors and deallocate memory.
        // Using Box::from_raw because the instance was allocated via Box::into_raw.
        let _ = unsafe { Box::from_raw(ptr.cast::<Self>()) };
    }

    pub(crate) fn dec_strong_ref_count(this: NonNull<Self>) {
        // SAFETY: this is valid because it comes from a valid Ref
        let state = unsafe { &(*this.as_ptr()).state };
        let new_count = state.dec_ref_count();

        if new_count == 0 {
            // Count just became 0, need to handle cleanup
            // SAFETY: We need mutable access to strong_wrapper for make_weak
            let state_mut = unsafe { &mut (*this.as_ptr()).state };
            match &mut state_mut.strong_wrapper {
                Some(wrapper) => {
                    // Pass a pointer to the State, not to the Instance.
                    // The weak callback will read drop_fn and this from the State.
                    let state_ptr =
                        unsafe { std::ptr::addr_of_mut!((*this.as_ptr()).state).cast::<c_void>() };
                    // isolate is guaranteed to be set when strong_wrapper is Some
                    let isolate = state_mut
                        .isolate
                        .expect("isolate must be set when wrapper exists")
                        .as_ptr();
                    // SAFETY: V8 FFI call
                    unsafe { wrapper.make_weak(isolate, state_ptr) };
                }
                None => {
                    // Reconstruct the Box to run destructors and deallocate memory.
                    // Using Box::from_raw because the instance was allocated via Box::into_raw.
                    let _ = unsafe { Box::from_raw(this.as_ptr()) };
                }
            }
        }
    }
}

/// Tracks the V8 wrapper object and reference count for a Rust resource instance.
///
/// When a resource is wrapped for JavaScript, `State` stores a strong V8 Global handle to the
/// wrapper object. The `strong_ref_count` tracks how many Rust `Ref<R>` handles exist. When the
/// count reaches zero and a wrapper exists, the Global is made weak so V8 can garbage collect
/// the wrapper and trigger cleanup via the weak callback.
pub struct State {
    /// Pointer to the owning Instance<R>. `None` until wrapped for JavaScript.
    /// Using `Option<NonNull>` makes the "not yet initialized" state explicit.
    this: Option<NonNull<c_void>>,
    /// V8 Global handle to the JavaScript wrapper object.
    strong_wrapper: Option<v8::Global<v8::Object>>,
    /// The V8 isolate this state is bound to. `None` until wrapped for JavaScript.
    isolate: Option<NonNull<v8::ffi::Isolate>>,
    /// Reference count using Cell for interior mutability (allows mutation through &self).
    /// This follows the same pattern as Rc in the standard library.
    strong_ref_count: Cell<usize>,
    /// Function pointer to drop the Instance<R>. Called from the weak callback
    /// triggered by the v8 garbage collector.
    drop_fn: Option<unsafe fn(*mut c_void)>,
    /// TypeId of the resource type R. Used to find the ResourceImpl<R> in Resources
    /// when the weak callback fires and we need to remove from instance tracking.
    type_id: Option<TypeId>,
}

impl Default for State {
    fn default() -> Self {
        Self {
            this: None,
            strong_wrapper: None,
            isolate: None,
            strong_ref_count: Cell::new(0),
            drop_fn: None,
            type_id: None,
        }
    }
}

impl State {
    pub fn new(drop_fn: unsafe fn(*mut c_void)) -> Self {
        Self {
            this: None,
            strong_wrapper: None,
            isolate: None,
            strong_ref_count: Cell::new(0),
            drop_fn: Some(drop_fn),
            type_id: None,
        }
    }

    /// Returns the isolate pointer if set.
    pub fn isolate(&self) -> Option<NonNull<v8::ffi::Isolate>> {
        self.isolate
    }

    /// Returns the TypeId of the resource type.
    pub fn type_id(&self) -> Option<TypeId> {
        self.type_id
    }

    /// Returns the pointer to the owning Instance, or `None` if not yet set.
    pub fn this_ptr(&self) -> Option<NonNull<c_void>> {
        self.this
    }

    /// Sets the pointer to the owning Instance.
    ///
    /// # Panics
    /// Panics in debug mode if `ptr` is null.
    pub(crate) fn set_this(&mut self, ptr: *mut c_void) {
        self.this = NonNull::new(ptr);
        debug_assert!(self.this.is_some(), "set_this called with null pointer");
    }

    /// Returns the drop function for cleaning up the Instance.
    pub fn drop_fn(&self) -> Option<unsafe fn(*mut c_void)> {
        self.drop_fn
    }

    /// Returns a reference to the strong wrapper if present.
    pub(crate) fn strong_wrapper(&self) -> Option<&v8::Global<v8::Object>> {
        self.strong_wrapper.as_ref()
    }

    /// Returns the current reference count.
    pub(crate) fn ref_count(&self) -> usize {
        self.strong_ref_count.get()
    }

    /// Increments the reference count and returns the new value.
    pub(crate) fn inc_ref_count(&self) -> usize {
        let count = self.strong_ref_count.get();
        self.strong_ref_count.set(count + 1);
        count + 1
    }

    /// Decrements the reference count and returns the new value.
    pub(crate) fn dec_ref_count(&self) -> usize {
        let count = self.strong_ref_count.get();
        debug_assert!(count > 0, "dec_ref_count called when count is already 0");
        self.strong_ref_count.set(count - 1);
        count - 1
    }

    /// Attaches a V8 wrapper object to this resource state.
    ///
    /// # Safety
    /// The caller must ensure:
    /// - `object` is a valid V8 local object handle
    /// - This method is called from the correct V8 isolate/context
    /// - `self.this` pointer remains valid until either the weak callback fires or `Realm::drop()` is called
    ///
    /// # Panics
    /// - Panics if a wrapper has already been attached (i.e., `strong_wrapper` is not `None`).
    /// - Panics if `this` has not been set.
    /// - Panics in debug mode if the isolate pointer is null.
    pub unsafe fn attach_wrapper<R: Resource + 'static>(
        &mut self,
        realm: &mut Realm,
        object: v8::Local<v8::Object>,
    ) where
        <R as Resource>::Template: 'static,
    {
        debug_assert!(self.strong_wrapper.is_none());

        self.strong_wrapper = Some(object.into());
        self.isolate = unsafe { Some(NonNull::new_unchecked(realm.isolate())) };
        self.type_id = Some(TypeId::of::<R>());
        debug_assert!(self.isolate.is_some(), "isolate pointer is null");

        let this_ptr = self.this.expect("this must be set before attach_wrapper");
        realm
            .get_resources::<R>()
            .add_instance(this_ptr.as_ptr().cast());
    }
}

/// A reference-counted handle to a Rust resource, analogous to `jsg::Ref<T>` in C++ JSG.
///
/// `Ref<R>` manages the reference count in `Instance<R>::state`. When all `Ref` handles are
/// dropped and a JavaScript wrapper exists, the V8 Global is made weak, allowing garbage
/// collection to reclaim the resource via the weak callback.
///
/// # Thread Safety
///
/// `Ref<T>` is **not thread-safe**. Resources are bound to the V8 isolate's thread and must
/// not be sent or accessed from other threads. The raw pointer field prevents automatic
/// `Send` and `Sync` implementations.
///
/// # Invariants
///
/// - `instance` is always a valid, non-null pointer to a heap-allocated `Instance<R>`
/// - The pointed-to instance was allocated via `Box::into_raw`
pub struct Ref<R: Resource> {
    instance: NonNull<Instance<R>>,
}

impl<R: Resource> Deref for Ref<R> {
    type Target = R;

    fn deref(&self) -> &Self::Target {
        // SAFETY: self.instance is always valid while Ref exists (invariant maintained by new/clone/drop)
        unsafe { &self.instance.as_ref().resource }
    }
}

impl<T: Resource> DerefMut for Ref<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        // SAFETY: self.instance is always valid while Ref exists, and we have &mut self
        unsafe { &mut self.instance.as_mut().resource }
    }
}

impl<R: Resource> Ref<R> {
    /// Creates a new `Ref` from an `Instance`.
    ///
    /// This allocates the instance on the heap and initializes the reference count to 1.
    pub(crate) fn new(instance: Instance<R>) -> Self {
        let instance = Box::new(instance);
        debug_assert_eq!(instance.state.ref_count(), 0);
        instance.state.inc_ref_count();
        // SAFETY: Box::into_raw never returns null
        let ptr = unsafe { NonNull::new_unchecked(Box::into_raw(instance)) };
        Self { instance: ptr }
    }

    /// Returns a raw pointer to the instance for FFI purposes.
    pub(crate) fn as_ptr(&self) -> *mut Instance<R> {
        self.instance.as_ptr()
    }
}

impl<T: Resource> Clone for Ref<T> {
    fn clone(&self) -> Self {
        // SAFETY: self.instance is always valid while Ref exists
        let state = unsafe { &self.instance.as_ref().state };
        state.inc_ref_count();
        Self {
            instance: self.instance,
        }
    }
}

impl<R: Resource> Drop for Ref<R> {
    fn drop(&mut self) {
        Instance::<R>::dec_strong_ref_count(self.instance);
    }
}

/// Wraps a Rust resource for exposure to JavaScript.
///
/// The caller must ensure V8 operations are performed within the correct isolate/context and
/// that the resource's lifetime is properly managed via the Realm.
///
/// # Panics
/// Panics if the internal state pointer fails to initialize (indicates a bug).
#[expect(clippy::needless_pass_by_value)] // Takes ownership intentionally
pub fn wrap<'a, R: Resource + 'static>(
    lock: &mut Lock,
    resource: Ref<R>,
) -> v8::Local<'a, v8::Value>
where
    <R as Resource>::Template: 'static,
{
    let instance_ptr = resource.as_ptr();
    // SAFETY: instance_ptr is valid because resource (Ref) maintains the invariant
    let state = unsafe { &mut (*instance_ptr).state };

    if let Some(wrapped) = state.strong_wrapper() {
        let local = wrapped.as_local(lock);
        if local.has_value() {
            return local.into();
        }
    }

    let resources = lock.get_realm().get_resources::<R>();
    let constructor = resources.get_constructor(lock);

    state.set_this(instance_ptr.cast());

    // SAFETY: V8 FFI calls require valid isolate and constructor
    // this_ptr() is guaranteed to be Some after set_this()
    let this_ptr = state.this_ptr().expect("this_ptr should be set").as_ptr();
    let wrapped_instance: v8::Local<'a, v8::Value> = unsafe {
        v8::Local::from_ffi(
            lock.isolate(),
            v8::ffi::wrap_resource(lock.isolate(), this_ptr as usize, constructor.as_ffi_ref()),
        )
    };
    // SAFETY: attach_wrapper requires valid V8 context
    unsafe { state.attach_wrapper::<R>(lock.get_realm(), wrapped_instance.clone().into()) };
    wrapped_instance
}

pub fn unwrap<'a, R: Resource>(lock: &'a mut Lock, value: v8::Local<v8::Value>) -> &'a mut R {
    let ptr =
        unsafe { v8::ffi::unwrap_resource(lock.isolate(), value.into_ffi()) as *mut Instance<R> };
    unsafe { &mut (*ptr).resource }
}

/// Stores per-type resource templates and instances, keyed by `TypeId`.
///
/// Each entry maps a Rust resource type to its `ResourceImpl<R>` which tracks the V8 function
/// template and all live instances of that type.
pub struct Resources {
    templates: HashMap<TypeId, Box<dyn ResourceCleanup>>,
}

impl Default for Resources {
    fn default() -> Self {
        Self {
            templates: HashMap::new(),
        }
    }
}

impl Drop for Resources {
    fn drop(&mut self) {
        // Clean up all tracked instances during Realm shutdown.
        // This follows the C++ HeapTracer::clearWrappers() pattern.
        for (_, resource_impl) in self.templates.iter_mut() {
            resource_impl.cleanup();
        }
    }
}

impl Resources {
    /// Gets or creates the resource tracking structure for a given resource type.
    ///
    /// # Panics
    /// Panics if a resource with the same `TypeId` but incompatible type was already registered.
    pub fn get_or_create<'a, R: Resource + 'static>(
        &'a mut self,
        _lock: &mut Lock,
    ) -> &'a mut ResourceImpl<R>
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

    /// Removes an instance from tracking by its TypeId.
    /// Called from the weak callback when V8 GC collects a wrapped resource.
    pub(crate) fn remove_instance_by_type_id(&mut self, type_id: TypeId, ptr: NonNull<c_void>) {
        if let Some(resource_impl) = self.templates.get_mut(&type_id) {
            resource_impl.remove_instance(ptr);
        }
    }
}

/// Tracks the V8 function template and all live instances for a single resource type.
///
/// The template is lazily initialized on first use. Instances are tracked so they can be
/// cleaned up when the Realm is dropped.
// TODO(soon): Find a better name for this struct.
pub struct ResourceImpl<R: Resource> {
    template: Option<R::Template>,
    instances: Vec<NonNull<Instance<R>>>,
}

impl<R: Resource> Default for ResourceImpl<R> {
    fn default() -> Self {
        Self {
            template: None,
            instances: Vec::new(),
        }
    }
}

impl<R: Resource + 'static> ResourceCleanup for ResourceImpl<R>
where
    <R as Resource>::Template: 'static,
{
    fn cleanup(&mut self) {
        // Drop all tracked instances directly.
        // This is called during Realm shutdown, so we don't wait for V8 GC.
        for instance_ptr in self.instances.drain(..) {
            // SAFETY: instance_ptr was created via Box::into_raw in Ref::new()
            // and is still valid since we're in the cleanup path (Realm is being dropped).
            let _ = unsafe { Box::from_raw(instance_ptr.as_ptr()) };
        }
    }

    fn remove_instance(&mut self, ptr: NonNull<c_void>) {
        // Remove the instance from tracking. Called from weak callback before dropping.
        let ptr = ptr.as_ptr().cast::<Instance<R>>();
        if let Some(pos) = self.instances.iter().position(|p| p.as_ptr() == ptr) {
            self.instances.swap_remove(pos);
        }
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

impl<R: Resource> ResourceImpl<R> {
    pub(crate) fn add_instance(&mut self, instance: *mut Instance<R>) {
        if let Some(ptr) = NonNull::new(instance) {
            self.instances.push(ptr);
        }
    }

    /// Returns the V8 function template constructor for this resource type.
    ///
    /// Lazily creates the template on first access.
    ///
    /// # Panics
    /// Panics if the template was not properly initialized (should never happen
    /// in normal usage since we initialize it lazily).
    pub fn get_constructor(&mut self, lock: &mut Lock) -> &v8::Global<v8::FunctionTemplate> {
        if self.template.is_none() {
            self.template = Some(R::Template::new(lock));
        }
        self.template
            .as_ref()
            .expect("Template not initialized")
            .get_constructor()
    }
}
