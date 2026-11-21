//! FFI island: the `RawWakerVTable` bridging the C++ `FutureWakerCell` into
//! `std::task::Waker`. `std`'s vtable ABI is four raw-pointer functions, so this file is where
//! ownership must round-trip through a raw `RawWaker` data slot — everywhere else (including the
//! whole C++ interface) waker ownership is a real `kj::Arc` handle. A genuine unsafe seam.
#![allow(
    unsafe_code,
    reason = "converts between `Waker` and the C++ waker through raw pointers"
)]

use std::mem::ManuallyDrop;
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
// The poll's Waker: a view of the PollWaker's cell that owns no reference
//
// C++ passes each poll a `&PollWaker`, which holds a strong reference to the event's cell until
// the poll returns. The `Waker` lent to `Future::poll()` is the cell's own `RawWaker` (the
// owned-cell vtable below, `data` = the cell) minus that reference, so it is the same `RawWaker`
// as every clone of it: `Waker::will_wake` holds between the poll's waker and the clones
// retained from earlier polls. Futures use that to keep a parked waker rather than replace it,
// and hyper's `want::Giver` wakes the waker it replaces, so without it a hyper connection would
// re-arm its own event on every poll and keep the loop from ever going idle.

/// The `Waker` for one poll driven by `waker`. `ManuallyDrop` because it carries no reference to
/// release: the `PollWaker` holds one for as long as the poll runs, and the poll bridge
/// (future.rs) drops this before returning.
pub fn lend(waker: &PollWaker) -> ManuallyDrop<Waker> {
    let raw = RawWaker::new(
        std::ptr::from_ref::<FutureWakerCell>(waker.cell()).cast::<()>(),
        &CELL_WAKER_VTABLE,
    );
    // Safety: the vtable functions below uphold the RawWaker contract (see the thread-safety
    // note above). Their `data` contract is a live cell, which the `PollWaker`'s reference
    // guarantees for as long as this view exists; `ManuallyDrop` keeps the view's drop from
    // releasing a reference it does not hold, while its clones take references of their own.
    ManuallyDrop::new(unsafe { Waker::from_raw(raw) })
}

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
/// `data` must point to a live `FutureWakerCell`: either one produced by [`cell_into_raw`]
/// whose strong reference this `RawWaker` still carries, or the cell of a `PollWaker` whose poll
/// is still running ([`lend`], whose view is never dropped through the vtable). These vtable
/// entries are only installed alongside such pointers, by [`lend`] and the functions below. It
/// is never null: both sources are references to a cell.
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
/// Same contract as [`cell_deref`], and the `RawWaker` must carry a strong reference (it does
/// unless it is a view from [`lend`], which is never dropped), which is released (the `RawWaker`
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

/// If `waker` is a KJ `FutureWakerCell` (the poll's waker from [`lend`], or a clone of one),
/// return the cell, borrowed from `waker` itself. Foreign Wakers return `None`: they expose no
/// `FuturePollEvent` to arm directly, so `RustPromiseAwaiter` takes its generic fallback path for
/// them.
pub fn try_waker_cell(waker: &Waker) -> Option<&FutureWakerCell> {
    // Identity comparison on the vtable pointer, NOT `==`: `RawWakerVTable`'s `PartialEq` compares
    // its function pointers, and Rust does not guarantee function-pointer equality across codegen
    // units (distinct fns may share an address, and a fn may have several). A different vtable that
    // happened to carry the same callbacks could pass a value compare, after which we would cast an
    // unrelated `data` pointer to `FutureWakerCell` -- an invalid unsafe cast. `ptr::eq` on the
    // vtable static is the reliable discriminator: CELL_WAKER_VTABLE is the one and only vtable
    // whose `data` is a cell.
    if std::ptr::eq(waker.vtable(), &raw const CELL_WAKER_VTABLE) {
        // Safety: Wakers carrying CELL_WAKER_VTABLE always point at a live cell (the
        // `cell_deref` contract), kept alive by `waker` itself or, for a lent view, by the
        // running poll that `waker` cannot outlive; the returned borrow is tied to `waker`.
        Some(unsafe { cell_deref(waker.data()) })
    } else {
        None
    }
}
