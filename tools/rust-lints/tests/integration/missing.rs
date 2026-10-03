// CXX generates unsafe declarations within its FFI module.
#![allow(unsafe_code)]

mod inherited_only {}

// The CXX bridge input is authored, even though it expands to generated code.
#[cxx::bridge]
mod ffi {
    extern "Rust" {
        fn answer() -> u32;
    }
}

fn answer() -> u32 {
    42
}
