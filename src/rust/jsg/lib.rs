#![feature(must_not_suspend)]
#![warn(must_not_suspend)]

use std::cell::Cell;
use std::rc::Rc;

pub type Error = String;
pub type Result<T> = std::result::Result<T, Error>;

pub mod ffi {

    pub struct Lock {}

    pub struct Value {}

    pub fn value_from_string(_lock: &Lock, _value: &str) -> Value {
        todo!()
    }

    pub struct Args {}

    impl Args {
        pub fn get_arg(&self, _index: usize) -> Value {
            todo!()
        }
    }

    pub fn string_from_value(_lock: &mut Lock, _v: Value) -> &str {
        todo!()
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
        C: FnOnce(Lock, I) -> Result<R>,
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
    pub fn register_resource<T>(&mut self)
    where
        T: Resource,
    {
        for _member in &T::members() {
            // register the member in v8
        }
    }
}

/// TODO: Implement memory_info(jsg::MemoryTracker)
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

pub type MethodCallbackImpl<S, R> =
    dyn FnMut(*mut S, *mut ffi::Lock, *mut ffi::Args) -> Result<R> + 'static;

pub enum Member<S: Sized> {
    Constructor,
    Method {
        name: &'static str,
        callback: Box<MethodCallbackImpl<S, ffi::Value>>,
    },
    Property {
        name: &'static str,
        getter: Box<MethodCallbackImpl<S, ffi::Value>>,
        setter: Option<Box<MethodCallbackImpl<S, ()>>>,
    },
    StaticMethod {
        name: &'static str,
        callback: Box<MethodCallbackImpl<(), ffi::Value>>,
    },
}

pub trait Resource: Type {
    fn members() -> Vec<Member<Self>>
    where
        Self: Sized;
}

pub trait Struct: Type {}
