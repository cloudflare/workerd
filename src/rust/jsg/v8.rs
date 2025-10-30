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
    members: Vec<Box<dyn IsolateMember>>,
}

pub struct Local<T> {
    ptr: *mut T,
}

pub struct LocalValue(u64);

impl LocalValue {
    pub(crate) unsafe fn new(value: u64) -> Self {
        LocalValue(value)
    }

    pub unsafe fn into_raw(self) -> u64 {
        self.0
    }
}

pub struct Lock {}
