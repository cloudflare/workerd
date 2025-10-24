#[cxx::bridge(namespace = "workerd::rust::jsg")]
pub mod ffi {
    extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type LocalValue;
        type V8Isolate;
    }

    extern "Rust" {
        type Isolate;

        unsafe fn isolate_created(isolate: *mut V8Isolate) -> Box<Isolate>;
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
    ptr: *mut ffi::V8Isolate,
    members: Vec<Box<dyn IsolateMember>>,
}

/// This method is called whenever a new isolate is created.
pub unsafe fn isolate_created(isolate: *mut ffi::V8Isolate) -> Box<Isolate> {
    Box::new(Isolate {
        ptr: isolate,
        members: Vec::new(),
    })
}
