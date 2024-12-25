use std::task::RawWaker;
use std::task::RawWakerVTable;
use std::task::Waker;

use crate::ffi::CxxWaker;

unsafe impl Send for CxxWaker {}
unsafe impl Sync for CxxWaker {}

fn deref_cxx_waker<'a>(data: *const ()) -> Option<&'a CxxWaker> {
    if !data.is_null() {
        let p = data as *const CxxWaker;
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

static CXX_WAKER_VTABLE: RawWakerVTable = RawWakerVTable::new(
    cxx_waker_clone,
    cxx_waker_wake,
    cxx_waker_wake_by_ref,
    cxx_waker_drop,
);

use crate::ffi::AwaitWaker;

unsafe impl Send for AwaitWaker {}
unsafe impl Sync for AwaitWaker {}

impl From<&AwaitWaker> for Waker {
    fn from(waker: &AwaitWaker) -> Self {
        let waker = RawWaker::new(waker as *const AwaitWaker as *const (), &AWAIT_WAKER_VTABLE);
        unsafe { Waker::from_raw(waker) }
    }
}

pub fn deref_await_waker<'a>(waker: &Waker) -> Option<&'a AwaitWaker> {
    if waker.vtable() == &AWAIT_WAKER_VTABLE {
        let data = waker.data();
        assert!(!data.is_null());
        let p = data as *const AwaitWaker;
        let await_waker = unsafe { &*p };

        if await_waker.is_current() {
            Some(await_waker)
        } else {
            None
        }
    } else {
        None
    }
}

static AWAIT_WAKER_VTABLE: RawWakerVTable = RawWakerVTable::new(
    cxx_waker_clone,
    cxx_waker_wake,
    cxx_waker_wake_by_ref,
    cxx_waker_drop,
);
