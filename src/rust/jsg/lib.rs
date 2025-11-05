#![feature(must_not_suspend)]
#![warn(must_not_suspend)]

use std::cell::Cell;
use std::marker::PhantomData;
use std::num::ParseIntError;
use std::rc::Rc;

use kj_rs::KjMaybe;

pub mod modules;
pub mod v8;

pub use crate::v8::LocalValue;

pub type Result<T, E = Error> = std::result::Result<T, E>;

#[cxx::bridge(namespace = "workerd::rust::jsg")]
pub mod ffi {

    pub struct ConstructorDescriptor {
        // todo: remove this
        name: String,
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

    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type Isolate = crate::v8::ffi::Isolate;
        type FunctionCallbackInfo;

        unsafe fn create_resource_template(
            isolate: *mut Isolate,
            descriptor: &ResourceDescriptor,
        ) -> usize /* v8::GlobalFunctionTemplate */;

        unsafe fn wrap_resource(
            isolate: *mut Isolate,
            resource: usize,    /* R* */
            constructor: usize, /* v8::LocalFunctionTemplate */
        ) -> usize /* v8::LocalValue */;

        unsafe fn unwrap_resource(
            isolate: *mut Isolate,
            value: usize, /* v8::LocalValue */
        ) -> usize /* R* */;
    }
}

fn get_resource_descriptor<R: Resource>() -> ffi::ResourceDescriptor {
    let mut descriptor = ffi::ResourceDescriptor {
        name: R::class_name().to_owned(),
        constructor: KjMaybe::None,
        methods: Vec::new(),
        static_methods: Vec::new(),
    };

    for m in R::members() {
        match m {
            Member::Constructor(_) => {
                descriptor.constructor = KjMaybe::Some(ffi::ConstructorDescriptor {
                    name: String::new(),
                });
            }
            Member::Method { name, callback } => {
                descriptor.methods.push(ffi::MethodDescriptor {
                    name: name.to_string(),
                    callback: callback as usize,
                });
            }
            Member::Property {
                name,
                getter_callback,
                setter_callback,
            } => todo!(),
            Member::StaticMethod { name, callback } => {
                descriptor.static_methods.push(ffi::StaticMethodDescriptor {
                    name: name.to_string(),
                    callback: callback as usize,
                });
            }
        }
    }

    descriptor
}

pub fn create_resource_constructor<R: Resource>(lock: &mut v8::Lock) -> v8::GlobalFunctionTemplate {
    unsafe {
        v8::GlobalFunctionTemplate::from_ffi(ffi::create_resource_template(
            lock.get_isolate(),
            &get_resource_descriptor::<R>(),
        ))
    }
}

// TODO: R needs to be a non-empty struct.
pub unsafe fn wrap_resource<'a, R: Resource + 'a, W: ResourceWrapper>(
    lock: &'a mut v8::Lock,
    resource: *mut R,
    wrapper: &W,
) -> v8::LocalValue<'a> {
    let r = unsafe { &mut *resource };
    let instance = r.js_instance(lock);
    match instance {
        Some(val) => val,
        None => {
            let constructor = wrapper.get_constructor(lock);
            let instance = unsafe {
                v8::LocalValue::from_ffi(
                    lock,
                    ffi::wrap_resource(lock.get_isolate(), resource as usize, constructor.to_ffi()),
                )
            };
            r.set_js_instance(lock, &instance);
            instance
        }
    }
}

// TODO: Take a look at the lifecycle.
pub fn unwrap_resource<'a, R: Resource>(
    lock: &'a mut v8::Lock,
    value: v8::LocalValue,
) -> &'a mut R {
    let ptr = unsafe { ffi::unwrap_resource(lock.get_isolate(), value.to_ffi()) as *mut R };
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

#[must_not_suspend]
pub struct Lock {}

impl Lock {
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
    pub fn as_mut<'a, 'b>(&'a mut self, _lock: &'b Lock) -> &'b mut T {
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

pub struct TypeRegistrar {}

impl TypeRegistrar {
    pub fn register_module<T>(&mut self, name: &str, module_type: modules::Type)
    where
        T: Resource,
    {
        for _member in &T::members() {
            // register the member in v8
        }
    }

    pub fn register_struct<T>(&mut self)
    where
        T: Struct,
    {
    }
}

/// TODO: Implement `memory_info(jsg::MemoryTracker)`
pub trait Type {
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

pub enum Member<S: Sized> {
    Constructor(PhantomData<S>),
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
    fn class_name() -> &'static str;
    fn members() -> Vec<Member<Self>>
    where
        Self: Sized;
    fn js_instance<'a>(&self, lock: &'a mut v8::Lock) -> Option<v8::LocalValue<'a>>;
    fn set_js_instance<'a>(&mut self, lock: &'a mut v8::Lock, instance: &v8::LocalValue<'a>);
}

pub trait ResourceWrapper {
    fn get_constructor<'a>(&self, isolate: &'a mut v8::Lock) -> v8::LocalFunctionTemplate;
}

pub trait Struct: Type {
    fn wrap(&self, lock: &v8::Lock) -> v8::LocalValue;
}
