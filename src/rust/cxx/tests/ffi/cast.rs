use std::os::raw::c_char;
use std::slice;

pub fn c_char_to_unsigned(slice: &[c_char]) -> &[u8] {
    let ptr = slice.as_ptr().cast::<u8>();
    let len = slice.len();
    // Safety: the test fixture provides valid bridge pointers and matching layouts.
    unsafe { slice::from_raw_parts(ptr, len) }
}

pub fn unsigned_to_c_char(slice: &[u8]) -> &[c_char] {
    let ptr = slice.as_ptr().cast::<c_char>();
    let len = slice.len();
    // Safety: the test fixture provides valid bridge pointers and matching layouts.
    unsafe { slice::from_raw_parts(ptr, len) }
}
