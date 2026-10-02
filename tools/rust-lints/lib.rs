#![feature(rustc_private)]
// Registration exports are the dynamic lint-library ABI.
#![allow(unsafe_code)]

extern crate rustc_ast;
extern crate rustc_driver;
extern crate rustc_errors;
extern crate rustc_expand;
extern crate rustc_lint;
extern crate rustc_session;
extern crate rustc_span;

mod explicit_unsafe_policy;

#[unsafe(no_mangle)]
pub extern "C" fn dylint_version() -> *mut std::os::raw::c_char {
    std::ffi::CString::new("0.1.0").unwrap().into_raw()
}

#[unsafe(no_mangle)]
pub fn register_lints(_sess: &rustc_session::Session, store: &mut rustc_lint::LintStore) {
    explicit_unsafe_policy::register(store);
}
