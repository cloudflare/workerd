use alloc::vec::Vec;
use core::ffi::c_char;
use core::mem;
use core::ptr;

use crate::c_char16;
use crate::rust_string::RustString;
use crate::rust_vec::RustVec;

macro_rules! rust_vec_shims {
    ($segment:expr, $ty:ty) => {
        const_assert_eq!(mem::size_of::<[usize; 3]>(), mem::size_of::<RustVec<$ty>>());
        const_assert_eq!(mem::size_of::<Vec<$ty>>(), mem::size_of::<RustVec<$ty>>());
        const_assert_eq!(mem::align_of::<Vec<$ty>>(), mem::align_of::<RustVec<$ty>>());

        const _: () = {
            #[unsafe(export_name = concat!("cxxbridge1$rust_vec$", $segment, "$new"))]
            unsafe extern "C" fn __new(this: *mut RustVec<$ty>) {
                // Safety: the bridge representation and ownership invariants satisfy this operation.
                unsafe { ptr::write(this, RustVec::new()) }
            }
            #[unsafe(export_name = concat!("cxxbridge1$rust_vec$", $segment, "$drop"))]
            unsafe extern "C" fn __drop(this: *mut RustVec<$ty>) {
                // Safety: the bridge representation and ownership invariants satisfy this operation.
                unsafe { ptr::drop_in_place(this) }
            }
            #[unsafe(export_name = concat!("cxxbridge1$rust_vec$", $segment, "$len"))]
            unsafe extern "C" fn __len(this: *const RustVec<$ty>) -> usize {
                // Safety: the bridge representation and ownership invariants satisfy this operation.
                unsafe { &*this }.len()
            }
            #[unsafe(export_name = concat!("cxxbridge1$rust_vec$", $segment, "$capacity"))]
            unsafe extern "C" fn __capacity(this: *const RustVec<$ty>) -> usize {
                // Safety: the bridge representation and ownership invariants satisfy this operation.
                unsafe { &*this }.capacity()
            }
            #[unsafe(export_name = concat!("cxxbridge1$rust_vec$", $segment, "$data"))]
            unsafe extern "C" fn __data(this: *const RustVec<$ty>) -> *const $ty {
                // Safety: the bridge representation and ownership invariants satisfy this operation.
                unsafe { &*this }.as_ptr()
            }
            #[unsafe(export_name = concat!("cxxbridge1$rust_vec$", $segment, "$reserve_total"))]
            unsafe extern "C" fn __reserve_total(this: *mut RustVec<$ty>, new_cap: usize) {
                // Safety: the bridge representation and ownership invariants satisfy this operation.
                unsafe { &mut *this }.reserve_total(new_cap);
            }
            #[unsafe(export_name = concat!("cxxbridge1$rust_vec$", $segment, "$set_len"))]
            unsafe extern "C" fn __set_len(this: *mut RustVec<$ty>, len: usize) {
                // Safety: the bridge representation and ownership invariants satisfy this operation.
                unsafe { (*this).set_len(len) }
            }
            #[unsafe(export_name = concat!("cxxbridge1$rust_vec$", $segment, "$truncate"))]
            unsafe extern "C" fn __truncate(this: *mut RustVec<$ty>, len: usize) {
                // Safety: the bridge representation and ownership invariants satisfy this operation.
                unsafe { (*this).truncate(len) }
            }
        };
    };
}

macro_rules! rust_vec_shims_for_primitive {
    ($ty:ident) => {
        rust_vec_shims!(stringify!($ty), $ty);
    };
}

rust_vec_shims_for_primitive!(bool);
rust_vec_shims_for_primitive!(u8);
rust_vec_shims_for_primitive!(u16);
rust_vec_shims_for_primitive!(u32);
rust_vec_shims_for_primitive!(u64);
rust_vec_shims_for_primitive!(usize);
rust_vec_shims_for_primitive!(i8);
rust_vec_shims_for_primitive!(i16);
rust_vec_shims_for_primitive!(i32);
rust_vec_shims_for_primitive!(i64);
rust_vec_shims_for_primitive!(isize);
rust_vec_shims_for_primitive!(f32);
rust_vec_shims_for_primitive!(f64);

rust_vec_shims!("char", c_char);
// c_char16 is an alias for u16, so these duplicate the u16 shims above under a
// second set of export names. The C++ side reaches them through
// rust::Vec<char16_t>, which mangles to the char16_t segment.
rust_vec_shims!("char16_t", c_char16);
rust_vec_shims!("string", RustString);
rust_vec_shims!("str", &str);
