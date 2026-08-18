use std::task::RawWaker;
use std::task::RawWakerVTable;
use std::task::Waker;

use crate::KjRc;
use crate::ffi::FutureWakerCell;
use crate::ffi::KjWaker;
use crate::ffi::PollWaker;

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
// Safety: the KJ bridge representation and ownership invariants satisfy this operation.
unsafe impl Send for KjWaker {}
// Safety: the KJ bridge representation and ownership invariants satisfy this operation.
unsafe impl Sync for KjWaker {}

impl From<&KjWaker> for Waker {
    fn from(waker: &KjWaker) -> Self {
        let waker = RawWaker::new(
            std::ptr::from_ref::<KjWaker>(waker).cast::<()>(),
            &KJ_WAKER_VTABLE,
        );
        // Safety: KjWaker's Rust-exposed interface is Send and Sync and its RawWakerVTable
        // implementation functions are all thread-safe.
        //
        // https://doc.rust-lang.org/std/task/struct.Waker.html#safety-1
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
        unsafe { Self::from_raw(waker) }
    }
}

// Helper function for use in KjWaker's RawWakerVTable implementation to factor out a tedious null
// pointer check.
fn deref_kj_waker<'a>(data: *const ()) -> Option<&'a KjWaker> {
    if data.is_null() {
        None
    } else {
        let p = data.cast::<KjWaker>();
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
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
        Some(unsafe { &*p })
    }
}

pub fn kj_waker_clone(data: *const ()) -> RawWaker {
    let new_data = if let Some(kj_waker) = deref_kj_waker(data) {
        kj_waker.clone_kj_waker().cast::<()>()
    } else {
        std::ptr::null()
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
pub fn try_into_kj_waker_ptr(waker: &Waker) -> *const KjWaker {
    if waker.vtable() == &KJ_WAKER_VTABLE {
        waker.data().cast::<KjWaker>()
    } else {
        std::ptr::null()
    }
}

// Owned-cell vtable: retained Wakers holding a strong reference to a FutureWakerCell
//
// `data` carries one strong reference (a disowned `KjRc<FutureWakerCell>`), or is null for the
// no-op Waker minted when `clone_cell()` had no event to bind (cannot normally happen in the
// single-thread world). Clone takes another reference; drop re-owns and releases the carried one.

/// Surrender the handle's strong reference into a bare pointer for a `RawWaker` data slot.
/// Reversed by `FutureWakerCell::reown` in [`cell_waker_drop`].
fn cell_into_raw(cell: KjRc<FutureWakerCell>) -> *const FutureWakerCell {
    let ptr = cell.get();
    std::mem::forget(cell);
    ptr
}

/// # Safety
///
/// `data` must be either null or a pointer produced by [`cell_into_raw`] whose strong reference
/// is still carried by this `RawWaker` (upheld by the `Waker`/`RawWaker` contract: these vtable
/// entries are only installed alongside such pointers, by [`poll_waker_clone`] and the functions
/// below).
unsafe fn cell_deref<'a>(data: *const ()) -> Option<&'a FutureWakerCell> {
    if data.is_null() {
        None
    } else {
        // Safety: non-null per the check; live per this fn's `# Safety` contract (the carried
        // strong reference keeps the cell alive).
        Some(unsafe { &*data.cast::<FutureWakerCell>() })
    }
}

/// # Safety
///
/// Same contract as [`cell_deref`].
unsafe fn cell_waker_clone(data: *const ()) -> RawWaker {
    // Safety: forwarded from this fn's `# Safety` contract.
    let new_data = if let Some(cell) = unsafe { cell_deref(data) } {
        cell_into_raw(cell.add_ref())
    } else {
        std::ptr::null()
    };
    RawWaker::new(new_data.cast::<()>(), &CELL_WAKER_VTABLE)
}

/// # Safety
///
/// Same contract as [`cell_deref`].
unsafe fn cell_waker_wake_by_ref(data: *const ()) {
    // Safety: forwarded from this fn's `# Safety` contract.
    if let Some(cell) = unsafe { cell_deref(data) } {
        cell.cell_wake_by_ref();
    }
}

/// # Safety
///
/// Same contract as [`cell_deref`], and the carried strong reference is released (the `RawWaker`
/// must not be used again — guaranteed by the `Waker` contract for `drop`).
unsafe fn cell_waker_drop(data: *const ()) {
    // Safety: forwarded from this fn's `# Safety` contract.
    if let Some(cell) = unsafe { cell_deref(data) } {
        // Safety: `data` carries a strong reference per this fn's `# Safety` contract; re-own it
        // and let the handle fall, releasing the reference.
        let _cell = unsafe { cell.reown() };
    }
}

/// # Safety
///
/// Same contract as [`cell_waker_drop`].
unsafe fn cell_waker_wake(data: *const ()) {
    // Safety: forwarded from this fn's `# Safety` contract; wake-then-release.
    unsafe {
        cell_waker_wake_by_ref(data);
        cell_waker_drop(data);
    }
}

static CELL_WAKER_VTABLE: RawWakerVTable = RawWakerVTable::new(
    cell_waker_clone,
    cell_waker_wake,
    cell_waker_wake_by_ref,
    cell_waker_drop,
);
