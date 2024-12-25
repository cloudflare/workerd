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

use crate::ffi::RootWaker;

unsafe impl Send for RootWaker {}
unsafe impl Sync for RootWaker {}

impl From<&RootWaker> for Waker {
    fn from(waker: &RootWaker) -> Self {
        let waker = RawWaker::new(waker as *const RootWaker as *const (), &ROOT_WAKER_VTABLE);
        unsafe { Waker::from_raw(waker) }
    }
}

pub fn deref_root_waker<'a>(waker: &Waker) -> Option<&'a RootWaker> {
    if waker.vtable() == &ROOT_WAKER_VTABLE {
        let data = waker.data();
        assert!(!data.is_null());
        let p = data as *const RootWaker;
        let root_waker = unsafe { &*p };

        if root_waker.is_current() {
            Some(root_waker)
        } else {
            None
        }
    } else {
        None
    }
}

static ROOT_WAKER_VTABLE: RawWakerVTable = RawWakerVTable::new(
    cxx_waker_clone,
    cxx_waker_wake,
    cxx_waker_wake_by_ref,
    cxx_waker_drop,
);
