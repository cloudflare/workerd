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

        pub unsafe fn new_local_object(isolate: *mut Isolate) -> usize /* LocalObject */;
        pub unsafe fn set_local_object_property(
            isolate: *mut Isolate,
            object: usize, /* LocalObject */
            key: *const u8,
            value: usize, /* LocalValue */
        );
    }
}

trait IsolateMember: Drop {
    /// Initialize the member with the given isolate.
    /// Nit: It will call something like "register()"
    fn init(&mut self, isolate: &mut Lock);
}

pub struct LocalValue(usize);

impl LocalValue {
    pub unsafe fn from_ffi(value: usize) -> Self {
        LocalValue(value)
    }

    pub unsafe fn to_ffi(self) -> usize {
        self.0
    }

    pub fn from_string(s: String) -> Self {
        todo!()
    }

    pub fn from_u8(u: u8) -> Self {
        todo!()
    }

    pub fn from_u32(u: u32) -> Self {
        todo!()
    }
}

pub struct LocalObject(usize);

impl LocalObject {
    pub unsafe fn new(lock: &Lock) -> Self {
        LocalObject(unsafe { ffi::new_local_object(lock.get_isolate()) })
    }

    pub unsafe fn set(&mut self, lock: &Lock, key: &str, value: LocalValue) {
        unsafe { ffi::set_local_object_property(lock.get_isolate(), self.0, key.as_ptr(), value.0) }
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
