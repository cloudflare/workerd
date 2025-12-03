use std::any::Any;
use std::any::TypeId;
use std::collections::HashMap;
use std::ffi::c_void;
use std::ops::Deref;
use std::ops::DerefMut;

use crate::Lock;
use crate::Realm;
use crate::Resource;
use crate::ResourceTemplate;
use crate::v8;

pub(crate) struct Instance<R: Resource> {
    resource: R,
    pub(crate) state: State,
}

impl<R: Resource> Instance<R> {
    #[expect(clippy::new_ret_no_self)]
    pub fn new(resource: R) -> Ref<R> {
        let instance = Self {
            resource,
            state: State::default(),
        };
        unsafe { Ref::new(instance) }
    }

    pub(crate) unsafe fn dec_strong_ref_count(this: *mut Self) {
        let state = unsafe { &mut (*this).state };
        state.strong_ref_count -= 1;
        if state.strong_ref_count == 0 {
            match &mut state.strong_wrapper {
                Some(wrapper) => {
                    let state_ptr = this.cast::<c_void>();
                    let isolate = state.isolate;

                    unsafe {
                        wrapper.make_weak(isolate, state_ptr, Self::weak_callback);
                    }
                }
                None => {
                    // Reconstruct the Box to run destructors and deallocate memory
                    let _ = unsafe { Box::from_raw(this) };
                }
            }
        }
    }

    fn weak_callback(_isolate: *mut v8::ffi::Isolate, data: usize) {
        // data is a pointer to Instance<R>
        let instance = data as *mut Self;

        // Global became weak when count reached 0. Meaning that
        // Rust side doesn't hold any reference to the instance anymore.
        // This means it could not have incremented the counter when GC
        // is triggered.
        assert_eq!(unsafe { (*instance).state.strong_ref_count }, 0);

        // Reconstruct the Box to run destructors and deallocate memory
        let _ = unsafe { Box::from_raw(instance) };
    }
}

/// Tracks the V8 wrapper object and reference count for a Rust resource instance.
///
/// When a resource is wrapped for JavaScript, `State` stores a strong V8 Global handle to the
/// wrapper object. The `strong_ref_count` tracks how many Rust `Ref<R>` handles exist. When the
/// count reaches zero and a wrapper exists, the Global is made weak so V8 can garbage collect
/// the wrapper and trigger cleanup via the weak callback.
pub struct State {
    /// This is *mut Instance<R>
    pub this: *mut c_void,
    pub strong_wrapper: Option<v8::Global<v8::Object>>,
    pub isolate: *mut v8::ffi::Isolate,
    pub strong_ref_count: usize,
}

impl Default for State {
    fn default() -> Self {
        Self {
            this: std::ptr::null_mut(),
            strong_wrapper: None,
            isolate: std::ptr::null_mut(),
            strong_ref_count: 0,
        }
    }
}

impl State {
    /// Attaches a V8 wrapper object to this resource state.
    ///
    /// # Safety
    /// The caller must ensure:
    /// - `object` is a valid V8 local object handle
    /// - This method is called from the correct V8 isolate/context
    /// - `self.this` pointer remains valid until either the weak callback fires or `Realm::drop()` is called
    ///
    /// # Panics
    /// Panics if a wrapper has already been attached (i.e., `strong_wrapper` is not `None`).
    pub unsafe fn attach_wrapper<R: Resource + 'static>(
        &mut self,
        realm: &mut Realm,
        object: v8::Local<v8::Object>,
    ) where
        <R as Resource>::Template: 'static,
    {
        assert!(self.strong_wrapper.is_none());

        self.strong_wrapper = Some(object.into());
        self.isolate = realm.isolate();

        realm.get_resources::<R>().add_instance(self.this.cast());
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
pub struct Ref<R: Resource> {
    instance: *mut Instance<R>,
}

impl<R: Resource> Deref for Ref<R> {
    type Target = R;

    fn deref(&self) -> &Self::Target {
        unsafe { &(*self.instance).resource }
    }
}

impl<T: Resource> DerefMut for Ref<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        unsafe { &mut (*self.instance).resource }
    }
}

impl<R: Resource> Ref<R> {
    pub(crate) unsafe fn new(instance: Instance<R>) -> Self {
        let mut instance = Box::new(instance);
        instance.state.strong_ref_count = 1;
        Self {
            instance: Box::into_raw(instance),
        }
    }
}

impl<T: Resource> Clone for Ref<T> {
    fn clone(&self) -> Self {
        unsafe {
            (*self.instance).state.strong_ref_count += 1;
        }
        Self {
            instance: self.instance,
        }
    }
}

impl<R: Resource> Drop for Ref<R> {
    fn drop(&mut self) {
        unsafe { Instance::<R>::dec_strong_ref_count(self.instance) };
    }
}

/// Wraps a Rust resource for exposure to JavaScript.
///
/// # Safety
/// The caller must ensure V8 operations are performed within the correct isolate/context and
/// that the resource's lifetime is properly managed via the Realm.
#[expect(clippy::needless_pass_by_value)] // Intentionally takes ownership to manage ref count
pub fn wrap<'a, R: Resource + 'static>(
    lock: &mut Lock,
    resource: Ref<R>,
) -> v8::Local<'a, v8::Value>
where
    <R as Resource>::Template: 'static,
{
    let instance = resource.instance;
    let state = unsafe { &mut (*instance).state };

    if let Some(ref wrapped) = state.strong_wrapper {
        let local = wrapped.as_local(lock);
        if local.has_value() {
            return local.into();
        }
    }

    match state.strong_wrapper.as_ref().map(|val| val.as_local(lock)) {
        Some(value) if value.has_value() => value.into(),
        _ => {
            let resources = lock.get_realm().get_resources::<R>();
            let constructor = resources.get_constructor(lock);

            state.this = instance.cast();

            let instance: v8::Local<'a, v8::Value> = unsafe {
                v8::Local::from_ffi(
                    lock.isolate(),
                    v8::ffi::wrap_resource(
                        lock.isolate(),
                        state.this as usize,
                        constructor.as_ffi_ref(),
                    ),
                )
            };
            unsafe { state.attach_wrapper::<R>(lock.get_realm(), instance.clone().into()) };
            instance
        }
    }
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
#[derive(Default)]
pub struct Resources {
    templates: HashMap<TypeId, Box<dyn Any>>,
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
            .downcast_mut::<ResourceImpl<R>>()
            .expect("Template type mismatch")
    }
}

/// Tracks the V8 function template and all live instances for a single resource type.
///
/// The template is lazily initialized on first use. Instances are tracked so they can be
/// cleaned up when the Realm is dropped.
// TODO(soon): Find a better name for this struct.
pub struct ResourceImpl<R: Resource> {
    template: Option<R::Template>,
    instances: Vec<*mut Instance<R>>,
}

impl<R: Resource> Default for ResourceImpl<R> {
    fn default() -> Self {
        Self {
            template: None,
            instances: Vec::new(),
        }
    }
}

impl<R: Resource> ResourceImpl<R> {
    pub(crate) fn add_instance(&mut self, instance: *mut Instance<R>) {
        self.instances.push(instance);
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
