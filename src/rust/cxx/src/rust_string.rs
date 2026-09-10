#![allow(missing_docs)]

use alloc::string::String;
use core::mem::MaybeUninit;
use core::mem::{self};
use core::ptr;

// ABI compatible with C++ rust::String (not necessarily alloc::string::String).
#[repr(C)]
pub struct RustString {
    repr: [MaybeUninit<usize>; mem::size_of::<String>() / mem::size_of::<usize>()],
}

impl RustString {
    pub fn from(s: String) -> Self {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        unsafe { mem::transmute::<String, Self>(s) }
    }

    pub fn from_ref(s: &String) -> &Self {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        unsafe { &*(s as *const String as *const Self) }
    }

    pub fn from_mut(s: &mut String) -> &mut Self {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        unsafe { &mut *(s as *mut String as *mut Self) }
    }

    pub fn into_string(self) -> String {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        unsafe { mem::transmute::<Self, String>(self) }
    }

    pub fn as_string(&self) -> &String {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        unsafe { &*(self as *const Self as *const String) }
    }

    pub fn as_mut_string(&mut self) -> &mut String {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        unsafe { &mut *(self as *mut Self as *mut String) }
    }
}

impl Drop for RustString {
    fn drop(&mut self) {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        unsafe { ptr::drop_in_place(self.as_mut_string()) }
    }
}

const_assert_eq!(mem::size_of::<[usize; 3]>(), mem::size_of::<RustString>());
const_assert_eq!(mem::size_of::<String>(), mem::size_of::<RustString>());
const_assert_eq!(mem::align_of::<String>(), mem::align_of::<RustString>());
