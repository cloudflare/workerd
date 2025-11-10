use std::cell::Cell;
use std::future::Future;
use std::num::ParseIntError;
use std::rc::Rc;

pub use jsg_macros::method;
pub use jsg_macros::resource;
pub use jsg_macros::r#struct;
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
pub unsafe fn wrap_resource<'a, R: Resource + 'a, W: ResourceWrapper>(
    lock: &mut Lock,
    resource: *mut R,
    wrapper: &mut W,
) -> v8::Local<'a, v8::Value> {
    let instance = wrapper.js_instance(lock);
    if let Some(val) = instance {
        val
    } else {
        let constructor = wrapper.get_constructor();
        let instance: v8::Local<'a, v8::Value> = unsafe {
            ffi::wrap_resource(
                lock.get_isolate(),
                resource as usize,
                constructor.as_ffi_ref(),
            )
            .into()
        };
        let cached_instance = instance.clone();
        wrapper.set_js_instance(lock, cached_instance);
        instance
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
        unsafe { v8::ffi::local_new_object(self.get_isolate()).into() }
    }

    // todo: Result?
    pub fn alloc<T>(&mut self, t: T) -> Ref<T> {
        Ref {
            t: Rc::new(Cell::new(t)),
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

pub struct Ref<T> {
    t: Rc<Cell<T>>,
}

impl<T> Ref<T> {
    pub fn as_mut<'b>(&mut self, _lock: &'b Lock) -> &'b mut T {
        todo!()
    }

    // self could potentially exist during all isolate lifetime, while
    // lock is created anew every time.
    // The resulting reference is bound by a lock lifetime to make
    // it impossible to hold across different invocations.
    pub fn as_ref<'a, 'b>(&'a self, _lock: &'b Lock) -> &'b T
    where
        'a: 'b,
    {
        todo!()
    }
}

impl<T> Clone for Ref<T> {
    fn clone(&self) -> Self {
        Self { t: self.t.clone() }
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

pub trait Resource: Type {
    fn members() -> Vec<Member>
    where
        Self: Sized;
}

pub trait ResourceWrapper {
    fn get_constructor(&self) -> &v8::Global<v8::FunctionTemplate>;
    fn js_instance<'a>(&self, lock: &mut Lock) -> Option<v8::Local<'a, v8::Value>>;
    fn set_js_instance(&mut self, lock: &mut Lock, instance: v8::Local<'_, v8::Value>);
}

pub trait Struct: Type {
    fn wrap<'a, 'b>(&self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a;
}
