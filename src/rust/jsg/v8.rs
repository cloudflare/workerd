#[cxx::bridge(namespace = "workerd::rust::jsg")]
pub mod ffi {
    extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type LocalValue;
        type Isolate;
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

/// This method is called whenever a new isolate is created.
pub fn isolate_created(isolate: *mut ffi::Isolate) -> Box<Isolate> {
    Box::new(Isolate {
        ptr: isolate,
        members: Vec::new(),
    })
}
