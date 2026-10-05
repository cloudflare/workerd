// Exposes libz-rs-sys (the memory-safe Rust implementation of the zlib C API)
// to C++ under zlib_rs_-prefixed C symbols. The unprefixed zlib names are owned
// by the routing layer (src/workerd/util/zlib-router.c++), which forwards to
// these or to the chromium implementation (Cr_z_*) based on the compression-rs
// autogate.

mod ffi;
