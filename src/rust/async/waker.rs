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

use crate::ffi::KjWaker;

// Safety: We use the type system to express the Sync nature of KjWaker in the cxx-rs FFI
// boundary. Specifically, we only allow invocations on const KjWakers, and in KJ C++, use of
// const-qualified functions is thread-safe by convention. Our implementations of KjWakers in C++
// respect this convention.
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

/// If `waker` wraps a `KjWaker` associated with the current thread's KJ event loop, return a
/// reference to the `KjWaker`.
pub fn deref_kj_waker<'a>(waker: &Waker) -> Option<&'a KjWaker> {
    if waker.vtable() == &KJ_WAKER_VTABLE {
        let data = waker.data();
        assert!(!data.is_null());
        let p = data as *const KjWaker;
        // Safety:
        // 1. p is guaranteed non-null by the assertion above.
        // 2. We possess a const borrow of the Waker which owns this pointer, so we are guaranteed
        //    the Waker is still alive. We assume the Waker was constructed correctly to begin with,
        //    and that therefore the pointer still points to valid memory.
        // 3. We do not read or write the KjWaker's memory, so there are no atomicity concerns nor
        //    interleaved pointer/reference access concerns.
        //
        // https://doc.rust-lang.org/std/ptr/index.html#safety
        let kj_waker = unsafe { &*p };

        if kj_waker.is_current() {
            Some(kj_waker)
        } else {
            None
        }
    } else {
        None
    }
}

// Define a separate `KJ_WAKER_VTABLE` object so we can distinguish KjWakers from other Waker
// objects. Since KjWakers have CxxWaker as a base class, we can re-use the CxxWaker's vtable
// functions.
static KJ_WAKER_VTABLE: RawWakerVTable = RawWakerVTable::new(
    cxx_waker_clone,
    cxx_waker_wake,
    cxx_waker_wake_by_ref,
    cxx_waker_drop,
);

// =======================================================================================
// RustWaker
//
// This is a wrapper around `std::task::Waker`, exposed to C++. We use it in `RustPromiseAwaiter`
// to allow KJ promises to be awaited using opaque Wakers implemented in Rust.

pub struct RustWaker {
    inner: Option<Waker>,
}

impl RustWaker {
    pub fn empty() -> RustWaker {
        RustWaker { inner: None }
    }

    pub fn set(&mut self, waker: &Waker) {
        if let Some(w) = &mut self.inner {
            w.clone_from(waker);
        } else {
            self.inner = Some(waker.clone());
        }
    }

    pub fn set_none(&mut self) {
        self.inner = None;
    }

    pub fn wake(&self) {
        // TODO(now): Should be able to call `.wake()` because we're only called on the event loop's
        //   thread.
        self.inner
            .as_ref()
            .expect("should have been set() before wake()")
            .wake_by_ref();
    }
}
