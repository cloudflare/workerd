use std::task::RawWaker;
use std::task::RawWakerVTable;
use std::task::Waker;

use crate::ffi::CxxWaker;

// Safety: We use the type system to express the Sync nature of CxxWaker in the cxx-rs FFI boundary.
// Specifically, we only allow invocations on const CxxWakers, and in KJ C++, use of const-qualified
// functions is thread-safe by convention. Our implementations of CxxWakers in C++ respect this
// convention.
//
// Note: Implementing these traits does not seem to be required for building, but the Waker
// documentation makes it clear Send and Sync are a requirement of the pointed-to type.
//
// https://doc.rust-lang.org/std/task/struct.RawWaker.html
// https://doc.rust-lang.org/std/task/struct.RawWakerVTable.html
unsafe impl Send for CxxWaker {}
unsafe impl Sync for CxxWaker {}

impl From<&CxxWaker> for Waker {
    fn from(waker: &CxxWaker) -> Self {
        let waker = RawWaker::new(waker as *const CxxWaker as *const (), &CXX_WAKER_VTABLE);
        // Safety: CxxWaker's Rust-exposed interface is Send and Sync and its RawWakerVTable
        // implementation functions are all thread-safe.
        //
        // https://doc.rust-lang.org/std/task/struct.Waker.html#safety-1
        unsafe { Waker::from_raw(waker) }
    }
}

// Helper function for use in CxxWaker's RawWakerVTable implementation to factor out a tedious null
// pointer check.
fn deref_cxx_waker<'a>(data: *const ()) -> Option<&'a CxxWaker> {
    if !data.is_null() {
        let p = data as *const CxxWaker;
        // Safety:
        // 1. p is guaranteed non-null by the check above.
        // 2. This function is only used in the implementations of our RawWakerVTable for CxxWaker.
        //    All vtable implementation functions are trivially guaranteed that their owning Waker
        //    object is still alive. We assume the Waker was constructed correctly to begin with,
        //    and that therefore the pointer still points to valid memory.
        // 3. We do not read or write the CxxWaker's memory, so there are no atomicity concerns nor
        //    interleaved pointer/reference access concerns.
        //
        // https://doc.rust-lang.org/std/ptr/index.html#safety
        Some(unsafe { &*p })
    } else {
        None
    }
}

pub fn cxx_waker_clone(data: *const ()) -> RawWaker {
    let new_data = if let Some(cxx_waker) = deref_cxx_waker(data) {
        cxx_waker.clone() as *const ()
    } else {
        std::ptr::null() as *const ()
    };
    RawWaker::new(new_data, &CXX_WAKER_VTABLE)
}

pub fn cxx_waker_wake(data: *const ()) {
    if let Some(cxx_waker) = deref_cxx_waker(data) {
        cxx_waker.wake();
    }
}

pub fn cxx_waker_wake_by_ref(data: *const ()) {
    if let Some(cxx_waker) = deref_cxx_waker(data) {
        cxx_waker.wake_by_ref();
    }
}

pub fn cxx_waker_drop(data: *const ()) {
    if let Some(cxx_waker) = deref_cxx_waker(data) {
        cxx_waker.drop();
    }
}

// `cxx_waker_clone()` uses this vtable to wrap new CxxWaker pointers.
static CXX_WAKER_VTABLE: RawWakerVTable = RawWakerVTable::new(
    cxx_waker_clone,
    cxx_waker_wake,
    cxx_waker_wake_by_ref,
    cxx_waker_drop,
);

/// If `waker` wraps a `CxxWaker`, return the `CxxWaker` pointer it was originally constructed with,
/// or null if `waker` does not wrap a `CxxWaker`. Note that the `CxxWaker` pointer originally used
/// to construct `waker` may itself by null.
pub fn try_into_cxx_waker_ptr<'a>(waker: &Waker) -> *const CxxWaker {
    if waker.vtable() == &CXX_WAKER_VTABLE {
        waker.data() as *const CxxWaker
    } else {
        std::ptr::null() as *const CxxWaker
    }
}

// =======================================================================================
// OptionWaker
//
// This is a wrapper around `std::task::Waker`, exposed to C++. We use it in `RustPromiseAwaiter`
// to allow KJ promises to be awaited using opaque Wakers implemented in Rust.

pub struct OptionWaker {
    inner: Option<Waker>,
}

impl OptionWaker {
    pub fn empty() -> OptionWaker {
        OptionWaker { inner: None }
    }

    pub fn set(&mut self, waker: &crate::WakerRef) {
        if let Some(w) = &mut self.inner {
            w.clone_from(waker.0);
        } else {
            self.inner = Some(waker.0.clone());
        }
    }

    pub fn set_none(&mut self) {
        self.inner = None;
    }

    pub fn wake(&mut self) {
        self.inner
            .take()
            .expect(
                "OptionWaker::set() should be called before RustPromiseAwaiter::poll(); \
                OptionWaker::wake() should be called at most once after poll()",
            )
            .wake();
    }
}
