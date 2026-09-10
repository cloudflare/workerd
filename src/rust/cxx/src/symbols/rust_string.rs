use alloc::borrow::ToOwned;
use alloc::string::String;
use core::mem::ManuallyDrop;
use core::mem::MaybeUninit;
use core::ptr;
use core::slice;
use core::str;

#[unsafe(export_name = "cxxbridge1$string$new")]
unsafe extern "C" fn string_new(this: &mut MaybeUninit<String>) {
    let this = this.as_mut_ptr();
    let new = String::new();
    // Safety: the bridge representation and ownership invariants satisfy this operation.
    unsafe { ptr::write(this, new) }
}

#[unsafe(export_name = "cxxbridge1$string$clone")]
unsafe extern "C" fn string_clone(this: &mut MaybeUninit<String>, other: &String) {
    let this = this.as_mut_ptr();
    let clone = other.clone();
    // Safety: the bridge representation and ownership invariants satisfy this operation.
    unsafe { ptr::write(this, clone) }
}

#[unsafe(export_name = "cxxbridge1$string$from_latin1")]
unsafe extern "C" fn string_from_latin1(
    this: &mut MaybeUninit<String>,
    ptr: *const u8,
    len: usize,
) {
    // Safety: the bridge representation and ownership invariants satisfy this operation.
    let slice = unsafe { slice::from_raw_parts(ptr, len) };
    let this = this.as_mut_ptr();
    let owned = slice.iter().map(|&c| c as char).collect();
    // Safety: the bridge representation and ownership invariants satisfy this operation.
    unsafe { ptr::write(this, owned) }
}

#[unsafe(export_name = "cxxbridge1$string$from_utf8")]
unsafe extern "C" fn string_from_utf8(
    this: &mut MaybeUninit<String>,
    ptr: *const u8,
    len: usize,
) -> bool {
    // Safety: the bridge representation and ownership invariants satisfy this operation.
    let slice = unsafe { slice::from_raw_parts(ptr, len) };
    match str::from_utf8(slice) {
        Ok(s) => {
            let this = this.as_mut_ptr();
            let owned = s.to_owned();
            // Safety: the bridge representation and ownership invariants satisfy this operation.
            unsafe { ptr::write(this, owned) }
            true
        }
        Err(_) => false,
    }
}

#[unsafe(export_name = "cxxbridge1$string$from_utf8_lossy")]
unsafe extern "C" fn string_from_utf8_lossy(
    this: &mut MaybeUninit<String>,
    ptr: *const u8,
    len: usize,
) {
    // Safety: the bridge representation and ownership invariants satisfy this operation.
    let slice = unsafe { slice::from_raw_parts(ptr, len) };
    let owned = String::from_utf8_lossy(slice).into_owned();
    let this = this.as_mut_ptr();
    // Safety: the bridge representation and ownership invariants satisfy this operation.
    unsafe { ptr::write(this, owned) }
}

#[unsafe(export_name = "cxxbridge1$string$from_utf16")]
unsafe extern "C" fn string_from_utf16(
    this: &mut MaybeUninit<String>,
    ptr: *const u16,
    len: usize,
) -> bool {
    // Safety: the bridge representation and ownership invariants satisfy this operation.
    let slice = unsafe { slice::from_raw_parts(ptr, len) };
    match String::from_utf16(slice) {
        Ok(s) => {
            let this = this.as_mut_ptr();
            // Safety: the bridge representation and ownership invariants satisfy this operation.
            unsafe { ptr::write(this, s) }
            true
        }
        Err(_) => false,
    }
}

#[unsafe(export_name = "cxxbridge1$string$from_utf16_lossy")]
unsafe extern "C" fn string_from_utf16_lossy(
    this: &mut MaybeUninit<String>,
    ptr: *const u16,
    len: usize,
) {
    // Safety: the bridge representation and ownership invariants satisfy this operation.
    let slice = unsafe { slice::from_raw_parts(ptr, len) };
    let owned = String::from_utf16_lossy(slice);
    let this = this.as_mut_ptr();
    // Safety: the bridge representation and ownership invariants satisfy this operation.
    unsafe { ptr::write(this, owned) }
}

#[unsafe(export_name = "cxxbridge1$string$drop")]
unsafe extern "C" fn string_drop(this: &mut ManuallyDrop<String>) {
    // Safety: the bridge representation and ownership invariants satisfy this operation.
    unsafe { ManuallyDrop::drop(this) }
}

#[unsafe(export_name = "cxxbridge1$string$ptr")]
unsafe extern "C" fn string_ptr(this: &String) -> *const u8 {
    this.as_ptr()
}

#[unsafe(export_name = "cxxbridge1$string$len")]
unsafe extern "C" fn string_len(this: &String) -> usize {
    this.len()
}

#[unsafe(export_name = "cxxbridge1$string$capacity")]
unsafe extern "C" fn string_capacity(this: &String) -> usize {
    this.capacity()
}

#[unsafe(export_name = "cxxbridge1$string$reserve_additional")]
unsafe extern "C" fn string_reserve_additional(this: &mut String, additional: usize) {
    this.reserve(additional);
}

#[unsafe(export_name = "cxxbridge1$string$reserve_total")]
unsafe extern "C" fn string_reserve_total(this: &mut String, new_cap: usize) {
    if new_cap > this.capacity() {
        let additional = new_cap - this.len();
        this.reserve(additional);
    }
}
