// Links libz-rs-sys (the memory-safe Rust implementation of the zlib C API)
// into the binary. The #[no_mangle] C symbols (deflate, inflate, ...) are
// pulled in by C++ callers referencing them; the re-export below makes the
// crate a required part of the link.
//
// Note the native chromium zlib linked elsewhere in workerd uses Cr_z_-mangled
// symbol names (chromeconf.h), so the unprefixed zlib API names are free.

pub use libz_rs_sys::*;
