#![allow(missing_docs)]

pub unsafe trait RustType {}
pub unsafe trait ImplBox {}
pub unsafe trait ImplVec {}

#[doc(hidden)]
pub fn verify_rust_type<T: RustType>() {}
