// Exposes libz-rs-sys (the memory-safe Rust implementation of the zlib C API)
// to C++ under zlib_rs_-prefixed C symbols. The unprefixed zlib names are owned
// by the routing layer (src/workerd/util/zlib-router.c++), which forwards to
// these or to the chromium implementation (Cr_z_*) based on the compression-rs
// autogate.
#![allow(non_snake_case)]
// Thin forwarders to the libz-rs-sys C API: each wrapper has exactly the safety contract of the
// zlib function it forwards to.
#![allow(clippy::missing_safety_doc)]
#![allow(clippy::undocumented_unsafe_blocks)]

use core::ffi::c_char;
use core::ffi::c_int;
use core::ffi::c_uint;
use core::ffi::c_ulong;

use libz_rs_sys::Bytef;
use libz_rs_sys::gz_headerp;
use libz_rs_sys::uInt;
use libz_rs_sys::z_stream;
use libz_rs_sys::z_streamp;

#[unsafe(no_mangle)]
pub extern "C" fn zlib_rs_zlibVersion() -> *const c_char {
    libz_rs_sys::zlibVersion()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_deflateInit_(
    strm: z_streamp,
    level: c_int,
    version: *const c_char,
    stream_size: c_int,
) -> c_int {
    unsafe { libz_rs_sys::deflateInit_(strm, level, version, stream_size) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_deflateInit2_(
    strm: z_streamp,
    level: c_int,
    method: c_int,
    windowBits: c_int,
    memLevel: c_int,
    strategy: c_int,
    version: *const c_char,
    stream_size: c_int,
) -> c_int {
    unsafe {
        libz_rs_sys::deflateInit2_(
            strm,
            level,
            method,
            windowBits,
            memLevel,
            strategy,
            version,
            stream_size,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_inflateInit_(
    strm: z_streamp,
    version: *const c_char,
    stream_size: c_int,
) -> c_int {
    unsafe { libz_rs_sys::inflateInit_(strm, version, stream_size) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_inflateInit2_(
    strm: z_streamp,
    windowBits: c_int,
    version: *const c_char,
    stream_size: c_int,
) -> c_int {
    unsafe { libz_rs_sys::inflateInit2_(strm, windowBits, version, stream_size) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_deflate(strm: *mut z_stream, flush: c_int) -> c_int {
    unsafe { libz_rs_sys::deflate(strm, flush) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_inflate(strm: *mut z_stream, flush: c_int) -> c_int {
    unsafe { libz_rs_sys::inflate(strm, flush) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_deflateEnd(strm: *mut z_stream) -> c_int {
    unsafe { libz_rs_sys::deflateEnd(strm) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_inflateEnd(strm: *mut z_stream) -> c_int {
    unsafe { libz_rs_sys::inflateEnd(strm) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_deflateReset(strm: *mut z_stream) -> c_int {
    unsafe { libz_rs_sys::deflateReset(strm) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_inflateReset(strm: *mut z_stream) -> c_int {
    unsafe { libz_rs_sys::inflateReset(strm) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_inflateReset2(strm: *mut z_stream, windowBits: c_int) -> c_int {
    unsafe { libz_rs_sys::inflateReset2(strm, windowBits) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_deflateParams(
    strm: z_streamp,
    level: c_int,
    strategy: c_int,
) -> c_int {
    unsafe { libz_rs_sys::deflateParams(strm, level, strategy) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_deflateSetDictionary(
    strm: z_streamp,
    dictionary: *const Bytef,
    dictLength: uInt,
) -> c_int {
    unsafe { libz_rs_sys::deflateSetDictionary(strm, dictionary, dictLength) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_inflateSetDictionary(
    strm: *mut z_stream,
    dictionary: *const u8,
    dictLength: c_uint,
) -> c_int {
    unsafe { libz_rs_sys::inflateSetDictionary(strm, dictionary, dictLength) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_deflateBound(strm: *mut z_stream, sourceLen: c_ulong) -> c_ulong {
    unsafe { libz_rs_sys::deflateBound(strm, sourceLen) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_deflateSetHeader(strm: *mut z_stream, head: gz_headerp) -> c_int {
    unsafe { libz_rs_sys::deflateSetHeader(strm, head) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_crc32(crc: c_ulong, buf: *const Bytef, len: uInt) -> c_ulong {
    unsafe { libz_rs_sys::crc32(crc, buf, len) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_adler32(adler: c_ulong, buf: *const Bytef, len: uInt) -> c_ulong {
    unsafe { libz_rs_sys::adler32(adler, buf, len) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_compress(
    dest: *mut Bytef,
    destLen: *mut c_ulong,
    source: *const Bytef,
    sourceLen: c_ulong,
) -> c_int {
    unsafe { libz_rs_sys::compress(dest, destLen, source, sourceLen) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_compress2(
    dest: *mut Bytef,
    destLen: *mut c_ulong,
    source: *const Bytef,
    sourceLen: c_ulong,
    level: c_int,
) -> c_int {
    unsafe { libz_rs_sys::compress2(dest, destLen, source, sourceLen, level) }
}

#[unsafe(no_mangle)]
pub extern "C" fn zlib_rs_compressBound(sourceLen: c_ulong) -> c_ulong {
    libz_rs_sys::compressBound(sourceLen)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zlib_rs_uncompress(
    dest: *mut u8,
    destLen: *mut c_ulong,
    source: *const u8,
    sourceLen: c_ulong,
) -> c_int {
    unsafe { libz_rs_sys::uncompress(dest, destLen, source, sourceLen) }
}
