// #![deny(unsafe_code)] is not an attribute.

mod inline {
    mod nested {}
}

#[path = "modules/outer.rs"]
mod external;

#[cfg_attr(any(), deny(unsafe_code))]
mod inactive_policy {}

#[warn(unsafe_code)]
mod warning {}

#[expect(unsafe_code)]
mod expectation {}

#[deny(warnings)]
mod warnings {}

#[deny(unsafe_op_in_unsafe_fn)]
mod different_lint {}

mod function_policy {
    #[deny(unsafe_code)]
    fn safe() {}
}

include!("modules/included_missing.rs");

#[allow(unsafe_code)]
mod parent {
    #[cxx::bridge]
    mod ffi {
        extern "Rust" {
            fn answer() -> u32;
        }
    }
    fn answer() -> u32 {
        42
    }
}
