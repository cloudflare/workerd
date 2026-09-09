#![allow(unsafe_code)]

use std::task::RawWaker;
use std::task::RawWakerVTable;
use std::task::Waker;

use crate::KjArc;
use crate::ffi::FutureWakerCell;
use crate::ffi::PollWaker;

impl From<&PollWaker> for Waker {
    fn from(waker: &PollWaker) -> Self {
        let raw = RawWaker::new(
            std::ptr::from_ref::<PollWaker>(waker).cast::<()>(),
            &POLL_WAKER_VTABLE,
        );
        // Safety: the PollWaker remains live until the returned Waker is dropped after poll(),
        // while cloned Wakers switch to the owned-cell vtable.
        unsafe { Self::from_raw(raw) }
    }
}

unsafe fn poll_waker_clone(data: *const ()) -> RawWaker {
    // Safety: this vtable is only installed with a live PollWaker pointer.
    let waker = unsafe { &*data.cast::<PollWaker>() };
    RawWaker::new(
        cell_into_raw(waker.clone_cell()).cast::<()>(),
        &CELL_WAKER_VTABLE,
    )
}

unsafe fn poll_waker_wake_by_ref(data: *const ()) {
    // Safety: this vtable is only installed with a live PollWaker pointer.
    let waker = unsafe { &*data.cast::<PollWaker>() };
    waker.wake_by_ref();
}

unsafe fn poll_waker_wake(data: *const ()) {
    // Safety: consuming a borrowed Waker owns nothing, so wake is wake_by_ref plus a no-op drop.
    unsafe { poll_waker_wake_by_ref(data) }
}

fn poll_waker_drop(_data: *const ()) {}

static POLL_WAKER_VTABLE: RawWakerVTable = RawWakerVTable::new(
    poll_waker_clone,
    poll_waker_wake,
    poll_waker_wake_by_ref,
    poll_waker_drop,
);

fn cell_into_raw(cell: KjArc<FutureWakerCell>) -> *const FutureWakerCell {
    let ptr = cell.get();
    std::mem::forget(cell);
    ptr
}

unsafe fn cell_deref<'a>(data: *const ()) -> &'a FutureWakerCell {
    debug_assert!(!data.is_null(), "owned-cell RawWaker with a null data slot");
    // Safety: every CELL_WAKER_VTABLE data slot carries a live strong reference.
    unsafe { &*data.cast::<FutureWakerCell>() }
}

unsafe fn cell_waker_clone(data: *const ()) -> RawWaker {
    // Safety: forwarded from the RawWaker vtable contract.
    let cell = unsafe { cell_deref(data) };
    RawWaker::new(
        cell_into_raw(cell.add_ref()).cast::<()>(),
        &CELL_WAKER_VTABLE,
    )
}

unsafe fn cell_waker_wake_by_ref(data: *const ()) {
    // Safety: forwarded from the RawWaker vtable contract.
    unsafe { cell_deref(data) }.wake_by_ref();
}

unsafe fn cell_waker_drop(data: *const ()) {
    // Safety: the data slot carries one surrendered strong reference, reclaimed exactly once.
    let _cell = unsafe { cell_deref(data).reown() };
}

unsafe fn cell_waker_wake(data: *const ()) {
    // Safety: wake the carried cell, then consume its strong reference.
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

pub fn try_poll_waker(waker: &Waker) -> Option<&PollWaker> {
    if waker.vtable() == &POLL_WAKER_VTABLE {
        // Safety: this vtable is only installed by From<&PollWaker>, and the returned borrow is
        // tied to the Waker that borrows the live PollWaker.
        Some(unsafe { &*waker.data().cast::<PollWaker>() })
    } else {
        None
    }
}
