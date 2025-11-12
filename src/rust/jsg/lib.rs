use std::cell::UnsafeCell;
use std::future::Future;
use std::num::ParseIntError;
use std::ops::Deref;
use std::ops::DerefMut;
use std::os::raw::c_void;
use std::rc::Rc;

use kj_rs::KjMaybe;
use v8::ffi;

pub mod modules;
pub mod v8;

pub type Result<T, E = Error> = std::result::Result<T, E>;

fn get_resource_descriptor<R: Resource>() -> v8::ffi::ResourceDescriptor {
    let mut descriptor = ffi::ResourceDescriptor {
        name: R::class_name().to_owned(),
        constructor: KjMaybe::None,
        methods: Vec::new(),
        static_methods: Vec::new(),
    };

    for m in R::members() {
        match m {
            Member::Constructor { callback } => {
                descriptor.constructor = KjMaybe::Some(ffi::ConstructorDescriptor {
                    callback: callback as usize,
                });
            }
            Member::Method { name, callback } => {
                descriptor.methods.push(ffi::MethodDescriptor {
                    name: name.to_owned(),
                    callback: callback as usize,
                });
            }
            Member::Property {
                name: _,
                getter_callback: _,
                setter_callback: _,
            } => todo!(),
            Member::StaticMethod { name, callback } => {
                descriptor.static_methods.push(ffi::StaticMethodDescriptor {
                    name: name.to_owned(),
                    callback: callback as usize,
                });
            }
        }
    }

    descriptor
}

pub fn create_resource_constructor<R: Resource>(
    lock: &mut Lock,
) -> v8::Global<v8::FunctionTemplate> {
    unsafe {
        ffi::create_resource_template(lock.get_isolate(), &get_resource_descriptor::<R>()).into()
    }
}

///
/// # Safety
/// The caller must ensure that `resource` points to a valid R instance.
pub unsafe fn wrap_resource<'a, R: Resource + 'a, RT: ResourceTemplate>(
    lock: &mut Lock,
    resource: Ref<R>,
    resource_template: &mut RT,
) -> v8::Local<'a, v8::Value> {
    let state = resource.get_state();
    let handle = state.wrapper.as_ref().map(|val| val.into_local(lock));

    match handle {
        Some(value) if value.has_value() => {
            todo!()
        }
        _ => {
            let constructor = resource_template.get_constructor();
            let instance: v8::Local<'a, v8::Value> = unsafe {
                // todo: can we get rid of this from_ffi call?
                v8::Local::from_ffi(
                    lock.get_isolate(),
                    ffi::wrap_resource(
                        lock.get_isolate(),
                        state.this as usize, // todo: who sets state.this?
                        constructor.as_ffi_ref(),
                        (*resource).get_drop_callback(),
                    ),
                )
            };
            unsafe { state.attach_wrapper(lock.get_isolate(), instance.clone().into(), false) };

            // state.wrapper = Some(instance.into_trace_reference(lock));
            // let cached_instance = instance.clone();
            // wrapper.set_js_instance(lock, cached_instance);
            instance
        }
    }
}

pub fn unwrap_resource<'a, R: Resource>(
    lock: &'a mut Lock,
    value: v8::Local<v8::Value>,
) -> &'a mut R {
    let ptr = unsafe { ffi::unwrap_resource(lock.get_isolate(), value.into_ffi()) as *mut R };
    unsafe { &mut *ptr }
}

pub struct Error {
    pub name: String,
    pub message: String,
}

impl Error {
    pub fn new(name: String, message: String) -> Self {
        Self { name, message }
    }
}

impl Default for Error {
    fn default() -> Self {
        Self {
            name: "Error".to_owned(),
            message: "An unknown error occurred".to_owned(),
        }
    }
}

impl From<ParseIntError> for Error {
    fn from(err: ParseIntError) -> Self {
        Self::new(
            "TypeError".to_owned(),
            format!("Failed to parse integer: {err}"),
        )
    }
}

pub struct Lock {
    isolate: *mut v8::ffi::Isolate,
}

impl Lock {
    /// # Safety
    /// The caller must ensure that `args` is a valid pointer to `FunctionCallbackInfo`.
    pub unsafe fn from_args(args: *mut v8::ffi::FunctionCallbackInfo) -> Self {
        unsafe { Self::from_isolate(v8::ffi::fci_get_isolate(args)) }
    }

    /// # Safety
    /// The caller must ensure that `isolate` is a valid pointer to an `Isolate`.
    pub unsafe fn from_isolate(isolate: *mut v8::ffi::Isolate) -> Self {
        Self {
            isolate: &raw mut *isolate,
        }
    }

    /// # Safety
    /// Returns a raw pointer to the isolate. The caller must ensure proper usage.
    pub unsafe fn get_isolate(&mut self) -> *mut v8::ffi::Isolate {
        self.isolate
    }

    pub fn new_object<'a>(&mut self) -> v8::Local<'a, v8::Object> {
        unsafe {
            v8::Local::from_ffi(
                self.get_isolate(),
                v8::ffi::local_new_object(self.get_isolate()),
            )
        }
    }

    pub fn await_io<F, C, I, R>(self, _fut: F, _callback: C) -> Result<R>
    where
        F: Future<Output = I>,
        C: FnOnce(Self, I) -> Result<R>,
    {
        todo!()
    }
}

pub struct Ref<T: Resource> {
    // todo: use SafeCell (maybe?)
    val: Rc<UnsafeCell<T>>,
}

impl<T: Resource> Deref for Ref<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        let ptr = self.val.get();
        unsafe { &*ptr }
    }
}

impl<T: Resource> DerefMut for Ref<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        let ptr = self.val.get();
        unsafe { &mut *ptr }
    }
}

impl<T: Resource> Ref<T> {
    pub fn new(t: T) -> Self {
        Self {
            val: Rc::new(UnsafeCell::new(t)),
        }
    }

    pub fn into_raw(r: Self) -> *mut T {
        UnsafeCell::raw_get(Rc::into_raw(r.val))
    }

    pub unsafe fn from_raw(this: *mut T) -> Self {
        Self {
            val: unsafe { Rc::from_raw(UnsafeCell::from_mut(&mut *this)) },
        }
    }
}

impl<T: Resource> Clone for Ref<T> {
    fn clone(&self) -> Self {
        Self {
            val: self.val.clone(),
        }
    }
}

/// TODO: Implement `memory_info(jsg::MemoryTracker)`
pub trait Type {
    fn class_name() -> &'static str;
    /// Same as jsgGetMemoryName
    fn memory_name() -> &'static str {
        std::any::type_name::<Self>()
    }
    /// Same as jsgGetMemorySelfSize
    fn memory_self_size() -> usize
    where
        Self: Sized,
    {
        std::mem::size_of::<Self>()
    }
}

pub enum Member {
    Constructor {
        callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
    Method {
        name: &'static str,
        callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
    Property {
        name: &'static str,
        getter_callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
        setter_callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
    StaticMethod {
        name: &'static str,
        callback: unsafe extern "C" fn(*mut v8::ffi::FunctionCallbackInfo),
    },
}

#[derive(Default)]
pub struct ResourceState {
    pub this: *mut c_void,
    pub shim: Option<v8::ffi::ResourceShim>,
    pub strong_wrapper: Option<v8::Global<v8::Object>>,
    pub isolate: *mut v8::ffi::Isolate,
    pub strong_ref_count: usize,
    pub wrapper: Option<v8::TracedReference<v8::Object>>,
}

impl ResourceState {
    pub unsafe fn attach_wrapper(
        &mut self,
        isolate: *mut v8::ffi::Isolate,
        object: v8::Local<v8::Object>,
        needs_gc_tracing: bool,
    ) {
        assert!(self.wrapper.is_none());
        assert!(self.strong_wrapper.is_none());

        self.wrapper = Some(object.into());
        self.isolate = isolate;

        // auto& tracer = HeapTracer::getTracer(isolate);
        // tracer.addWrapper({}, *this);

        // Deal with tags.

        // v8::Object::Wrap<WRAPPABLE_TAG>(isolate, object, tracer.allocateShim(*this));
        //
        todo!();

        if self.strong_ref_count > 0 {
            self.strong_wrapper = Some(object.into());

            todo!()

            // GcVisitor visitor(*this, kj::none);
            // jsgVisitForGc(visitor);
        }
    }
}

pub trait Resource: Type {
    fn members() -> Vec<Member>
    where
        Self: Sized;
    fn get_drop_callback(&self) -> usize;
    fn get_state(&self) -> &mut ResourceState;
}

pub trait ResourceTemplate {
    fn new(lock: &mut Lock) -> Self;
    fn get_constructor(&self) -> &v8::Global<v8::FunctionTemplate>;
}

pub trait Struct: Type {
    fn wrap<'a, 'b>(&self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a;
}

pub unsafe fn drop_resource<R: Resource>(_isolate: *mut ffi::Isolate, this: *mut c_void) {
    let this = this.cast::<R>();
    let this = unsafe { Ref::from_raw(this) };
    drop(this);
}
