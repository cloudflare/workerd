#[cxx::bridge(namespace = "workerd::rust::jsg")]
pub mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type Isolate;
        type FunctionCallbackInfo;

        pub unsafe fn get_isolate(args: *mut FunctionCallbackInfo) -> *mut Isolate;
        pub unsafe fn get_this(args: *mut FunctionCallbackInfo) -> usize /* LocalObject */;
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
    }
}

trait IsolateMember: Drop {
    /// Initialize the member with the given isolate.
    /// Nit: It will call something like "register()"
    fn init(&mut self, isolate: &mut Lock);
}

pub struct Local<T> {
    ptr: *mut T,
}

pub struct LocalValue(usize);

impl LocalValue {
    pub unsafe fn from_ffi(value: usize) -> Self {
        LocalValue(value)
    }

    pub unsafe fn to_ffi(self) -> usize {
        self.0
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

    pub fn get_this(&self) -> LocalValue {
        unsafe { LocalValue::from_ffi(ffi::get_this(self.0)) }
    }
}
