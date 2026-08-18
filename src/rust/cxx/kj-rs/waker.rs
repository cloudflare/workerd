//! FFI island (see crate-root `#![deny(unsafe_code)]`): the two `RawWakerVTable`s bridging the
//! C++ wakers into `std::task::Waker`. `std`'s vtable ABI is four raw-pointer functions, so this
//! file is where ownership must round-trip through a raw `RawWaker` data slot — everywhere else
//! (including the whole C++ interface) waker ownership is a real `kj::Rc` handle. A genuine unsafe
//! seam.
#![allow(unsafe_code)]

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
        cell.wake_by_ref();
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

// Thread-safety: `std::task::Waker` documents that the vtable functions must be thread-safe.
// The bridge is single-threaded — no waker is ever woken, cloned, or dropped from another thread
// (cross-thread producers interpose a same-thread forwarding task instead; see kj-rs-io's
// `resolve_host`) — so these vtables uphold that contract degenerately: every call happens on the
// owning event loop's thread.

// =======================================================================================
// Borrowed vtable: Wakers lending out the PollWaker C++ passes to `Future::poll()`
//
// `data` is the `&PollWaker` the Waker was built from — borrowed, never null, and alive for the
// duration of the poll (the Waker is created and dropped inside the poll bridge in future.rs).
// Dropping such a Waker frees nothing; cloning it takes a real strong reference to the event's
// FutureWakerCell and switches to the owned-cell vtable below.

impl From<&PollWaker> for Waker {
    fn from(waker: &PollWaker) -> Self {
        let raw = RawWaker::new(
            std::ptr::from_ref::<PollWaker>(waker).cast::<()>(),
            &POLL_WAKER_VTABLE,
        );
        // Safety: the vtable functions below uphold the RawWaker contract (see the thread-safety
        // note above); `data` outlives the Waker because future.rs drops the Waker before poll
        // returns.
        unsafe { Self::from_raw(raw) }
    }
}

/// # Safety
///
/// `data` must be the pointer a [`From<&PollWaker>`] conversion was made with, still live per the
/// `RawWaker` contract (upheld because these Wakers only exist within a single `poll` call).
unsafe fn poll_waker_clone(data: *const ()) -> RawWaker {
    // Safety: forwarded from this fn's `# Safety` contract.
    let waker = unsafe { &*data.cast::<PollWaker>() };
    let cell: Option<KjRc<FutureWakerCell>> = waker.clone_cell().into();
    RawWaker::new(
        cell.map_or(std::ptr::null(), cell_into_raw).cast::<()>(),
        &CELL_WAKER_VTABLE,
    )
}

/// # Safety
///
/// Same contract as [`poll_waker_clone`].
unsafe fn poll_waker_wake_by_ref(data: *const ()) {
    // Safety: forwarded from this fn's `# Safety` contract.
    let waker = unsafe { &*data.cast::<PollWaker>() };
    waker.wake_by_ref();
}

/// # Safety
///
/// Same contract as [`poll_waker_clone`]. Consuming a borrowed Waker owns nothing, so `wake` is
/// just `wake_by_ref` (the paired drop is a no-op).
unsafe fn poll_waker_wake(data: *const ()) {
    // Safety: forwarded from this fn's `# Safety` contract.
    unsafe { poll_waker_wake_by_ref(data) }
}

fn poll_waker_drop(_data: *const ()) {
    // No-op: the PollWaker is stack-owned by the C++ poll scope; this Waker only borrowed it.
}

static POLL_WAKER_VTABLE: RawWakerVTable = RawWakerVTable::new(
    poll_waker_clone,
    poll_waker_wake,
    poll_waker_wake_by_ref,
    poll_waker_drop,
);

/// If `waker` lends out a C++ `PollWaker` (borrowed vtable above), return a reference to it,
/// borrowed from `waker` itself. Owned-cell and foreign Wakers both return `None`: neither
/// exposes a `FuturePollEvent` to arm directly, so `RustPromiseAwaiter` takes its generic
/// fallback path for them.
pub fn try_poll_waker(waker: &Waker) -> Option<&PollWaker> {
    if waker.vtable() == &POLL_WAKER_VTABLE {
        // Safety: Wakers carrying POLL_WAKER_VTABLE are only ever built by `From<&PollWaker>`
        // above, so `data` is a `&PollWaker` that outlives `waker` (the PollWaker is stack-owned
        // by the C++ poll driving this call); the returned borrow is tied to `waker`'s lifetime.
        Some(unsafe { &*waker.data().cast::<PollWaker>() })
    } else {
        None
    }
}
