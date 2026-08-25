// Links libz-rs-sys (the memory-safe Rust implementation of the zlib C API)
// into the binary. The #[no_mangle] C symbols (deflate, inflate, ...) are
// pulled in by C++ callers referencing them; the re-export below makes the
// crate a required part of the link.
pub use libz_rs_sys::*;
