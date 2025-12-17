use std::cell::Cell;
use std::cell::UnsafeCell;
use std::marker::PhantomData;
use std::ops::Deref;
use std::ptr::NonNull;

use crate::GarbageCollected;
use crate::Lock;
use crate::Resource;
use crate::ResourceTemplate;
use crate::v8;

pub(crate) struct Instance<R: Resource> {
    pub resource: R,
    wrapper: Option<v8::TracedReference<v8::Object>>,
    strong_refcount: Cell<u32>,
}

impl<R: Resource + 'static> Instance<R> {
    pub fn alloc(lock: &mut Lock, resource: R) -> Ref<R> {
        let instance = Self {
            resource,
            wrapper: None,
            strong_refcount: Cell::new(0),
        };
        let ptr = unsafe { v8::cppgc::make_garbage_collected(lock.isolate(), instance) };
        Ref::from_ptr(&ptr)
    }

    pub(crate) fn traced_reference(&self) -> Option<&v8::TracedReference<v8::Object>> {
        self.wrapper.as_ref()
    }

    pub(crate) fn set_wrapper(&mut self, object: v8::Local<v8::Object>) {
        debug_assert!(self.wrapper.is_none());
        self.wrapper = Some(object.into());
    }

    fn increment_ref(&self) {
        self.strong_refcount.set(self.strong_refcount.get() + 1);
    }

    fn decrement_ref(&self) {
        let count = self.strong_refcount.get();
        debug_assert!(count > 0);
        self.strong_refcount.set(count - 1);
    }
}

impl<R: Resource> v8::GcParent for Instance<R> {
    fn strong_refcount(&self) -> u32 {
        self.strong_refcount.get()
    }

    fn has_wrapper(&self) -> bool {
        self.wrapper.is_some()
    }
}

impl<R: Resource> GarbageCollected for Instance<R> {
    fn trace(&self, visitor: &mut v8::GcVisitor<'_>) {
        let mut child_visitor = visitor.with_parent(self);
        self.resource.trace(&mut child_visitor);

        if let Some(wrapper) = &self.wrapper {
            visitor.trace(wrapper);
        }
    }

    fn get_name(&self) -> Option<&'static std::ffi::CStr> {
        None
    }
}

/// A reference to a resource that dynamically switches between strong and traced modes.
pub struct Ref<R: Resource> {
    storage: UnsafeCell<RefStorage>,
    _marker: PhantomData<R>,
}

enum RefStorage {
    Strong(v8::cppgc::Handle),
    Traced(v8::cppgc::Member),
}

impl RefStorage {
    fn get(&self) -> Option<NonNull<v8::ffi::RustResource>> {
        match self {
            Self::Strong(handle) => handle.get_resource(),
            Self::Traced(member) => member.get(),
        }
    }
}

impl<R: Resource> Ref<R> {
    fn from_ptr(ptr: &v8::cppgc::Ptr<Instance<R>>) -> Self {
        ptr.get().increment_ref();
        Self {
            storage: UnsafeCell::new(RefStorage::Strong(unsafe {
                v8::cppgc::Handle::from_resource(ptr.as_rust_resource())
            })),
            _marker: PhantomData,
        }
    }

    pub(crate) fn instance_ptr(&self) -> *mut Instance<R> {
        // SAFETY: single-threaded V8 isolate
        unsafe { &*self.storage.get() }
            .get()
            .map(|r| unsafe { v8::cppgc::rust_resource_to_instance::<Instance<R>>(r.as_ptr()) })
            .expect("Ref handle should always be valid")
    }

    fn instance(&self) -> &Instance<R> {
        unsafe { &*self.instance_ptr() }
    }

    pub(crate) fn rust_resource(&self) -> Option<NonNull<v8::ffi::RustResource>> {
        // SAFETY: single-threaded V8 isolate
        unsafe { &*self.storage.get() }.get()
    }

    pub(crate) fn visit(&self, visitor: &mut v8::GcVisitor<'_>) -> &Instance<R> {
        let instance = self.instance();

        if let Some(parent) = visitor.parent() {
            let should_be_strong = parent.strong_refcount() > 0 && !parent.has_wrapper();

            // SAFETY: single-threaded V8 isolate
            unsafe {
                match &*self.storage.get() {
                    RefStorage::Traced(member) if should_be_strong => {
                        if let Some(resource) = member.get() {
                            instance.increment_ref();
                            *self.storage.get() = RefStorage::Strong(
                                v8::cppgc::Handle::from_resource(resource.as_ptr()),
                            );
                        }
                    }
                    RefStorage::Strong(handle) if !should_be_strong => {
                        if let Some(resource) = handle.get_resource() {
                            instance.decrement_ref();
                            *self.storage.get() =
                                RefStorage::Traced(v8::cppgc::Member::from_resource(resource));
                        }
                    }
                    _ => {}
                }
            }
        }

        // SAFETY: single-threaded V8 isolate
        unsafe {
            if let RefStorage::Traced(member) = &*self.storage.get() {
                visitor.trace_member(member);
            }
        }

        instance
    }
}

impl<R: Resource> Deref for Ref<R> {
    type Target = R;

    fn deref(&self) -> &Self::Target {
        // SAFETY: Ref maintains invariant that handle is valid
        unsafe { &(*self.instance_ptr()).resource }
    }
}

impl<R: Resource> Clone for Ref<R> {
    fn clone(&self) -> Self {
        self.instance().increment_ref();
        Self {
            storage: UnsafeCell::new(RefStorage::Strong(
                self.rust_resource()
                    .map(|r| unsafe { v8::cppgc::Handle::from_resource(r.as_ptr()) })
                    .unwrap_or_default(),
            )),
            _marker: PhantomData,
        }
    }
}

impl<R: Resource> Drop for Ref<R> {
    fn drop(&mut self) {
        if let RefStorage::Strong(handle) = self.storage.get_mut()
            && let Some(r) = handle.get_resource()
        {
            let instance =
                unsafe { v8::cppgc::rust_resource_to_instance::<Instance<R>>(r.as_ptr()) };
            unsafe { (*instance).decrement_ref() };
        }
    }
}

/// A weak reference to a resource that doesn't prevent garbage collection.
pub struct WeakRef<R: Resource> {
    weak_handle: v8::cppgc::WeakHandle,
    _marker: PhantomData<R>,
}

impl<R: Resource> WeakRef<R> {
    pub fn get(&self) -> Option<&R> {
        self.weak_handle.get().map(|r| {
            let instance_ptr =
                unsafe { v8::cppgc::rust_resource_to_instance::<Instance<R>>(r.as_ptr()) };
            unsafe { &(*instance_ptr).resource }
        })
    }

    pub fn is_alive(&self) -> bool {
        self.weak_handle.is_alive()
    }

    pub fn upgrade(&self) -> Option<Ref<R>> {
        self.weak_handle.get().map(|r| {
            let instance_ptr =
                unsafe { v8::cppgc::rust_resource_to_instance::<Instance<R>>(r.as_ptr()) };
            unsafe { (*instance_ptr).increment_ref() };

            Ref {
                storage: UnsafeCell::new(RefStorage::Strong(unsafe {
                    v8::cppgc::Handle::from_resource(r.as_ptr())
                })),
                _marker: PhantomData,
            }
        })
    }
}

impl<R: Resource> From<&Ref<R>> for WeakRef<R> {
    fn from(r: &Ref<R>) -> Self {
        Self {
            weak_handle: r
                .rust_resource()
                .map(|rr| unsafe { v8::cppgc::WeakHandle::from_resource(rr.as_ptr()) })
                .unwrap_or_default(),
            _marker: PhantomData,
        }
    }
}

impl<R: Resource> Clone for WeakRef<R> {
    fn clone(&self) -> Self {
        Self {
            weak_handle: self
                .weak_handle
                .get()
                .map(|r| unsafe { v8::cppgc::WeakHandle::from_resource(r.as_ptr()) })
                .unwrap_or_default(),
            _marker: PhantomData,
        }
    }
}

impl<R: Resource> Default for WeakRef<R> {
    fn default() -> Self {
        Self {
            weak_handle: v8::cppgc::WeakHandle::default(),
            _marker: PhantomData,
        }
    }
}

#[expect(clippy::needless_pass_by_value)]
pub fn wrap<'a, R: Resource + 'static>(
    lock: &mut Lock,
    resource: Ref<R>,
) -> v8::Local<'a, v8::Value>
where
    <R as Resource>::Template: 'static,
{
    let instance_ptr = resource.instance_ptr();
    // SAFETY: Ref guarantees instance is valid
    let instance = unsafe { &mut *instance_ptr };

    if let Some(traced) = instance.traced_reference() {
        let local = traced.get(lock);
        if local.has_value() {
            return local.into();
        }
    }

    let resources = lock.realm().get_resources::<R>();
    let constructor = resources.get_constructor(lock);

    let wrapped: v8::Local<'a, v8::Value> = unsafe {
        v8::Local::from_ffi(
            lock.isolate(),
            v8::ffi::wrap_resource(
                lock.isolate().as_ptr(),
                instance_ptr as usize,
                constructor.as_ffi_ref(),
            ),
        )
    };

    instance.set_wrapper(wrapped.clone().into());

    wrapped
}

/// # Safety
/// The `value` must be a valid JavaScript wrapper for type `R`.
pub unsafe fn unwrap<'a, R: Resource>(
    lock: &'a mut Lock,
    value: v8::Local<v8::Value>,
) -> &'a mut R {
    let ptr = unsafe {
        v8::ffi::unwrap_resource(lock.isolate().as_ptr(), value.into_ffi()) as *mut Instance<R>
    };
    unsafe { &mut (*ptr).resource }
}

/// # Safety
/// The `value` must be a valid JavaScript wrapper for type `R`.
pub unsafe fn unwrap_ref<R: Resource>(lock: &mut Lock, value: v8::Local<v8::Value>) -> Ref<R> {
    let instance_ptr = unsafe {
        v8::ffi::unwrap_resource(lock.isolate().as_ptr(), value.into_ffi()) as *mut Instance<R>
    };

    unsafe { (*instance_ptr).increment_ref() };

    let rust_resource = unsafe { v8::cppgc::instance_to_rust_resource(instance_ptr) };

    Ref {
        storage: UnsafeCell::new(RefStorage::Strong(unsafe {
            v8::cppgc::Handle::from_resource(rust_resource)
        })),
        _marker: PhantomData,
    }
}

pub struct ResourceImpl<R: Resource> {
    template: Option<R::Template>,
    _marker: PhantomData<R>,
}

impl<R: Resource> Default for ResourceImpl<R> {
    fn default() -> Self {
        Self {
            template: None,
            _marker: PhantomData,
        }
    }
}

impl<R: Resource> ResourceImpl<R> {
    pub fn get_constructor(&mut self, lock: &mut Lock) -> &v8::Global<v8::FunctionTemplate> {
        self.template
            .get_or_insert_with(|| R::Template::new(lock))
            .get_constructor()
    }
}
