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
    fn init(&mut self, isolate: &mut Isolate);
}

/// Represents a V8 isolate.
/// It needs to have the same lifetime as the v8 Isolate.
pub struct Isolate {
    ptr: *mut ffi::Isolate,
}

impl Isolate {
    pub unsafe fn from_ffi(isolate: *mut ffi::Isolate) -> Self {
        Isolate { ptr: isolate }
    }

    pub unsafe fn to_ffi(&mut self) -> *mut ffi::Isolate {
        self.ptr
    }
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

pub struct Lock {}

pub struct GlobalFunctionTemplate(usize);

impl GlobalFunctionTemplate {
    pub fn as_local(&self, isolate: &mut Isolate) -> LocalFunctionTemplate {
        unsafe {
            LocalFunctionTemplate::from_ffi(ffi::global_function_template_as_local(
                isolate.to_ffi(),
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
