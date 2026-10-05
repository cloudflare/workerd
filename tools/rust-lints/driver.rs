#![feature(rustc_private)]
#![deny(unsafe_code)]

extern crate rustc_driver;

fn main() {
    if let Err(error) = dylint_driver::dylint_driver(&std::env::args_os().collect::<Vec<_>>()) {
        eprintln!("{error:#}");
        std::process::exit(1);
    }
}
