use std::task::RawWaker;
use std::task::RawWakerVTable;
use std::task::Waker;

use crate::ffi::KjWaker;

// Safety: We use the type system to express the Sync nature of KjWaker in the cxx-rs FFI boundary.
// Specifically, we only allow invocations on const KjWakers, and in KJ C++, use of const-qualified
// functions is thread-safe by convention. Our implementations of KjWakers in C++ respect this
// convention.
//
// Note: Implementing these traits does not seem to be required for building, but the Waker
// documentation makes it clear Send and Sync are a requirement of the pointed-to type.
//
// https://doc.rust-lang.org/std/task/struct.RawWaker.html
// https://doc.rust-lang.org/std/task/struct.RawWakerVTable.html
unsafe impl Send for KjWaker {}
unsafe impl Sync for KjWaker {}

impl From<&KjWaker> for Waker {
    fn from(waker: &KjWaker) -> Self {
        let waker = RawWaker::new(waker as *const KjWaker as *const (), &KJ_WAKER_VTABLE);
        // Safety: KjWaker's Rust-exposed interface is Send and Sync and its RawWakerVTable
        // implementation functions are all thread-safe.
        //
        // https://doc.rust-lang.org/std/task/struct.Waker.html#safety-1
        unsafe { Waker::from_raw(waker) }
    }
}

// Helper function for use in KjWaker's RawWakerVTable implementation to factor out a tedious null
// pointer check.
fn deref_kj_waker<'a>(data: *const ()) -> Option<&'a KjWaker> {
    if !data.is_null() {
        let p = data as *const KjWaker;
        // Safety:
        // 1. p is guaranteed non-null by the check above.
        // 2. This function is only used in the implementations of our RawWakerVTable for KjWaker.
        //    All vtable implementation functions are trivially guaranteed that their owning Waker
        //    object is still alive. We assume the Waker was constructed correctly to begin with,
        //    and that therefore the pointer still points to valid memory.
        // 3. We do not read or write the KjWaker's memory, so there are no atomicity concerns nor
        //    interleaved pointer/reference access concerns.
        //
        // https://doc.rust-lang.org/std/ptr/index.html#safety
        Some(unsafe { &*p })
    } else {
        None
    }
}

pub fn kj_waker_clone(data: *const ()) -> RawWaker {
    let new_data = if let Some(kj_waker) = deref_kj_waker(data) {
        kj_waker.clone() as *const ()
    } else {
        std::ptr::null() as *const ()
    };
    RawWaker::new(new_data, &KJ_WAKER_VTABLE)
}

pub fn kj_waker_wake(data: *const ()) {
    if let Some(kj_waker) = deref_kj_waker(data) {
        kj_waker.wake();
    }
}

pub fn kj_waker_wake_by_ref(data: *const ()) {
    if let Some(kj_waker) = deref_kj_waker(data) {
        kj_waker.wake_by_ref();
    }
}

pub fn kj_waker_drop(data: *const ()) {
    if let Some(kj_waker) = deref_kj_waker(data) {
        kj_waker.drop();
    }
}

static KJ_WAKER_VTABLE: RawWakerVTable = RawWakerVTable::new(
    kj_waker_clone,
    kj_waker_wake,
    kj_waker_wake_by_ref,
    kj_waker_drop,
);

/// If `waker` wraps a `KjWaker`, return the `KjWaker` pointer it was originally constructed with,
/// or null if `waker` does not wrap a `KjWaker`. Note that the `KjWaker` pointer originally used
/// to construct `waker` may itself by null.
pub fn try_into_kj_waker_ptr<'a>(waker: &Waker) -> *const KjWaker {
    if waker.vtable() == &KJ_WAKER_VTABLE {
        waker.data() as *const KjWaker
    } else {
        std::ptr::null() as *const KjWaker
    }
}
