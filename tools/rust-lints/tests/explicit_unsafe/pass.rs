#![deny(unsafe_code)]

mod inline {
    #![deny(unsafe_code)]
    mod nested {
        #![forbid(unsafe_code)]
    }
}

#[path = "modules/directory/mod.rs"]
mod directory;
#[path = "modules/inner.rs"]
mod inner;
#[deny(unsafe_code)]
#[path = "modules/outer.rs"]
mod outer;

#[path = "modules/inactive_file.rs"]
mod inactive_file;

#[cfg_attr(policy_active, deny(unsafe_code))]
mod conditional {}

#[cfg(any())]
mod inactive {
    mod inactive_child {}
}

macro_rules! generate {
    () => {
        mod generated_helper {}
    };
}
generate!();

macro_rules! generate_named {
    ($name:ident) => {
        mod $name {}
    };
}
generate_named!(named_generated_helper);

include!("modules/included_pass.rs");

// The CXX bridge generates unsafe FFI declarations.
#[cxx::bridge]
mod ffi {
    #![allow(unsafe_code)]
    extern "Rust" {
        fn answer() -> u32;
    }
}
fn answer() -> u32 {
    42
}

#[cfg(test)]
mod tests {
    #[test]
    fn safe() {}
}
