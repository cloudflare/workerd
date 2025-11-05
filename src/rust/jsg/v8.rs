use std::marker::PhantomData;

#[cxx::bridge(namespace = "workerd::rust::jsg")]
pub mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type Isolate;
        type FunctionCallbackInfo;

        pub unsafe fn get_isolate(args: *mut FunctionCallbackInfo) -> *mut Isolate;
        pub unsafe fn get_this(args: *mut FunctionCallbackInfo) -> usize /* LocalValue */;
        pub unsafe fn get_length(args: *mut FunctionCallbackInfo) -> usize;
        pub unsafe fn get_arg(args: *mut FunctionCallbackInfo, index: usize) -> usize /* LocalValue */;
        pub unsafe fn unwrap_string(
            isolate: *mut Isolate,
            value: usize, /* LocalValue */
        ) -> String;
        pub unsafe fn global_function_template_as_local(
            isolate: *mut Isolate,
            value: usize, /* GlobalFunctionTemplate */
        ) -> usize /* LocalFunctionTemplate */;

        pub unsafe fn new_local_number(isolate: *mut Isolate, value: f64) -> usize /* LocalValue */;

        pub unsafe fn new_local_string(isolate: *mut Isolate, value: &str) -> usize /* LocalValue */;

        pub unsafe fn new_local_object(isolate: *mut Isolate) -> usize /* LocalObject */;
        pub unsafe fn set_local_object_property(
            isolate: *mut Isolate,
            object: usize, /* LocalObject */
            key: &str,
            value: usize, /* LocalValue */
        );

        pub unsafe fn set_return_value(
            args: *mut FunctionCallbackInfo,
            value: usize, /* LocalValue */
        );

        pub unsafe fn local_to_global_value(
            isolate: *mut Isolate,
            value: usize, /* LocalValue */
        ) -> usize /* GlobalValue */;
        pub unsafe fn global_to_local_value(
            isolate: *mut Isolate,
            value: usize, /* GlobalValue */
        ) -> usize /* LocalValue */;
    }
}

trait IsolateMember: Drop {
    /// Initialize the member with the given isolate.
    /// Nit: It will call something like "register()"
    fn init(&mut self, isolate: &mut Lock);
}

pub struct LocalValue<'a>(usize, PhantomData<&'a ()>);

impl<'a> LocalValue<'a> {
    pub unsafe fn from_ffi(lock: &'a mut Lock, value: usize) -> Self {
        LocalValue(value, PhantomData)
    }

    pub unsafe fn to_ffi(self) -> usize {
        self.0
    }

    pub fn from_string(lock: &'a mut Lock, value: &str) -> Self {
        unsafe { Self::from_ffi(lock, ffi::new_local_string(lock.get_isolate(), value)) }
    }

    pub fn from_f64(lock: &'a mut Lock, value: f64) -> Self {
        unsafe { Self::from_ffi(lock, ffi::new_local_number(lock.get_isolate(), value)) }
    }

    pub fn to_global(&self, lock: &'a mut Lock) -> GlobalValue {
        unsafe { GlobalValue::from_ffi(ffi::local_to_global_value(lock.get_isolate(), self.0)) }
    }
}

impl<'a> From<LocalObject<'a>> for LocalValue<'a> {
    fn from(value: LocalObject<'a>) -> Self {
        LocalValue(value.0, PhantomData)
    }
}

pub struct LocalObject<'a>(usize, PhantomData<&'a ()>);

impl<'a> LocalObject<'a> {
    pub unsafe fn new(lock: &'a Lock) -> Self {
        LocalObject(
            unsafe { ffi::new_local_object(lock.get_isolate()) },
            PhantomData,
        )
    }

    pub unsafe fn set(&mut self, lock: &'a Lock, key: &str, value: LocalValue) {
        unsafe { ffi::set_local_object_property(lock.get_isolate(), self.0, key, value.0) }
    }
}

pub struct GlobalValue(usize);

impl GlobalValue {
    pub unsafe fn from_ffi(value: usize) -> Self {
        GlobalValue(value)
    }

    pub fn to_local<'a>(&self, lock: &'a mut Lock) -> LocalValue<'a> {
        unsafe {
            LocalValue::from_ffi(lock, ffi::global_to_local_value(lock.get_isolate(), self.0))
        }
    }
}

pub struct Lock {
    ptr: *mut ffi::Isolate,
}

impl Lock {
    pub unsafe fn from_args(args: *mut ffi::FunctionCallbackInfo) -> Self {
        unsafe {
            Lock {
                ptr: ffi::get_isolate(args),
            }
        }
    }

    pub unsafe fn from_isolate(isolate: *mut ffi::Isolate) -> Self {
        Lock { ptr: isolate }
    }

    pub unsafe fn get_isolate(&self) -> *mut ffi::Isolate {
        self.ptr
    }
}

pub struct GlobalFunctionTemplate(usize);

impl GlobalFunctionTemplate {
    pub fn as_local(&self, lock: &mut Lock) -> LocalFunctionTemplate {
        unsafe {
            LocalFunctionTemplate::from_ffi(ffi::global_function_template_as_local(
                lock.get_isolate(),
                self.0,
            ))
        }
    }

    pub unsafe fn from_ffi(value: usize) -> Self {
        GlobalFunctionTemplate(value)
    }
}

pub struct LocalFunctionTemplate(usize);

impl LocalFunctionTemplate {
    pub unsafe fn from_ffi(value: usize) -> Self {
        LocalFunctionTemplate(value)
    }

    pub unsafe fn to_ffi(self) -> usize {
        self.0
    }
}

pub struct FunctionCallbackInfo(*mut ffi::FunctionCallbackInfo);

impl FunctionCallbackInfo {
    pub unsafe fn from_ffi(info: *mut ffi::FunctionCallbackInfo) -> Self {
        FunctionCallbackInfo(info)
    }

    pub fn get_this<'a>(&self, lock: &'a mut Lock) -> LocalValue<'a> {
        unsafe { LocalValue::from_ffi(lock, ffi::get_this(self.0)) }
    }

    pub fn set_return_value(&self, value: LocalValue) {
        unsafe {
            ffi::set_return_value(self.0, value.0);
        }
    }
}
