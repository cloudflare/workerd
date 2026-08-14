//! FFI island (see crate-root `#![deny(unsafe_code)]`): the two `RawWakerVTable`s bridging the
//! C++ wakers into `std::task::Waker`. `std`'s vtable ABI is four raw-pointer functions, so this
//! file is where ownership must round-trip through a raw `RawWaker` data slot — everywhere else
//! (including the whole C++ interface) waker ownership is a real `kj::Arc` handle. A genuine
//! unsafe seam.
#![allow(unsafe_code)]

use std::task::RawWaker;
use std::task::RawWakerVTable;
use std::task::Waker;

use crate::KjArc;
use crate::ffi::FutureWakerCell;
use crate::ffi::PollWaker;

// Thread-safety: `std::task::Waker` documents that the vtable functions must be thread-safe, and
// `Waker: Send + Sync` means safe Rust may clone, wake, and drop these from any thread. The
// vtables uphold that for real: clone/drop are atomic refcount operations on the
// `FutureWakerCell` (`Send + Sync`; see the impls in ffi.rs), and `wakeByRef` routes wakes
// through an owning-executor check on the C++ side — same-thread wakes arm the event directly,
// foreign-thread wakes go through a cross-thread fulfiller (waker.h).

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
    RawWaker::new(
        cell_into_raw(waker.clone_cell()).cast::<()>(),
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

// =======================================================================================
// Owned-cell vtable: retained Wakers holding a strong reference to a FutureWakerCell
//
// `data` carries one strong reference (a disowned `KjArc<FutureWakerCell>`). Clone takes another
// reference; drop re-owns and releases the carried one. All of it is safe from any thread: the
// refcount is atomic and the cell's wake is executor-routed (waker.h).

/// Surrender the handle's strong reference into a bare pointer for a `RawWaker` data slot.
/// Reversed by `FutureWakerCell::reown` in [`cell_waker_drop`].
fn cell_into_raw(cell: KjArc<FutureWakerCell>) -> *const FutureWakerCell {
    let ptr = cell.get();
    std::mem::forget(cell);
    ptr
}

/// # Safety
///
/// `data` must be a pointer produced by [`cell_into_raw`] whose strong reference is still
/// carried by this `RawWaker` (upheld by the `Waker`/`RawWaker` contract: these vtable entries
/// are only installed alongside such pointers, by [`poll_waker_clone`] and the functions below).
/// It is never null: `cell_into_raw` takes a live `KjArc`, whose pointee is `NonNull`.
unsafe fn cell_deref<'a>(data: *const ()) -> &'a FutureWakerCell {
    debug_assert!(!data.is_null(), "owned-cell RawWaker with a null data slot");
    // Safety: live and non-null per this fn's `# Safety` contract (the carried strong reference
    // keeps the cell alive).
    unsafe { &*data.cast::<FutureWakerCell>() }
}

/// # Safety
///
/// Same contract as [`cell_deref`].
unsafe fn cell_waker_clone(data: *const ()) -> RawWaker {
    // Safety: forwarded from this fn's `# Safety` contract.
    let cell = unsafe { cell_deref(data) };
    RawWaker::new(
        cell_into_raw(cell.add_ref()).cast::<()>(),
        &CELL_WAKER_VTABLE,
    )
}

/// # Safety
///
/// Same contract as [`cell_deref`].
unsafe fn cell_waker_wake_by_ref(data: *const ()) {
    // Safety: forwarded from this fn's `# Safety` contract.
    unsafe { cell_deref(data) }.wake_by_ref();
}

/// # Safety
///
/// Same contract as [`cell_deref`], and the carried strong reference is released (the `RawWaker`
/// must not be used again -- guaranteed by the `Waker` contract for `drop`).
unsafe fn cell_waker_drop(data: *const ()) {
    // Safety: forwarded from this fn's `# Safety` contract; `data` carries a strong reference,
    // so re-own it and let the handle fall, releasing the reference.
    let _cell = unsafe { cell_deref(data).reown() };
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
