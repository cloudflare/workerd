// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust port of `workerd::api::node::i18n::transcode`
//! (`src/workerd/api/node/i18n.c++`), the engine behind `node:buffer`'s
//! `transcode()`. Selected at runtime by the `NODEJS_I18N_RUST` autogate; when
//! the gate is off, the C++ implementation is used instead. The two paths are
//! byte-for-byte and error-message identical by construction: [`dispatch`]
//! ports the C++ dispatch/sizing/truncation logic to Rust, while [`shim`]
//! calls the exact same ICU and simdutf primitives the C++ path uses, through
//! the C++ shim in `shim.h` / `shim.c++`.

use jsg::Lock;
use jsg::ToJS;
use jsg::v8;

mod dispatch;
mod error;
mod shim;

#[cxx::bridge(namespace = "workerd::rust::i18n")]
mod ffi {
    /// The four encodings `i18n::transcode` supports. Mirrors the
    /// transcodable subset of `workerd::api::node::Encoding`
    /// (`src/workerd/api/node/i18n.h`). `src/workerd/api/node/i18n.c++` maps
    /// into this type through a `fromImpl` overload that rejects the
    /// non-transcodable `BASE64`, `BASE64URL`, and `HEX` variants before ever
    /// calling into Rust (see R10).
    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    #[repr(u8)]
    enum Encoding {
        Ascii,
        Latin1,
        Utf8,
        Utf16Le,
    }

    unsafe extern "C++" {
        include!("workerd/rust/i18n/shim.h");

        type Converter;

        fn open_converter(name: &str) -> UniquePtr<Converter>;
        fn max_char_size(self: &Converter) -> usize;
        fn min_char_size(self: &Converter) -> usize;
        fn set_subst_chars(self: &Converter, substitute: &str);

        fn convert_ex(to: &Converter, from: &Converter, source: &[u8], target: &mut [u8]) -> i64;
        fn from_uchars(to: &Converter, source: &[u8], target: &mut [u8]) -> i64;

        fn convert_latin1_to_utf16(source: &[u8], target: &mut [u8]) -> usize;
        fn utf16_length_from_utf8(source: &[u8]) -> usize;
        fn convert_utf8_to_utf16le(source: &[u8], target: &mut [u8]) -> usize;
        fn utf8_length_from_utf16le(source: &[u8]) -> usize;
        fn convert_utf16le_to_utf8(source: &[u8], target: &mut [u8]) -> usize;
    }

    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    extern "Rust" {
        /// Transcodes `source` from `from_encoding` to `to_encoding`, matching
        /// `workerd::api::node::i18n::transcode`. Returns a `MaybeLocal`
        /// naming a `Uint8Array`, or an empty `MaybeLocal` with a JS exception
        /// already scheduled on `isolate` if transcoding fails.
        ///
        /// # Safety
        /// `isolate` must be a valid pointer to a locked `v8::Isolate` with an
        /// active `HandleScope`.
        unsafe fn transcode(
            isolate: *mut Isolate,
            source: &[u8],
            from_encoding: Encoding,
            to_encoding: Encoding,
        ) -> MaybeLocal;
    }
}

/// # Safety
/// `isolate` must be a valid pointer to a locked `v8::Isolate` with an active
/// `HandleScope`.
unsafe fn transcode(
    isolate: *mut ffi::Isolate,
    source: &[u8],
    from_encoding: ffi::Encoding,
    to_encoding: ffi::Encoding,
) -> ffi::MaybeLocal {
    // SAFETY: forwarded from this function's own safety contract -- the C++
    // caller (`i18n::transcode` in `i18n.c++`) guarantees `isolate` is valid,
    // locked, and has an active HandleScope.
    let mut lock = unsafe { Lock::from_isolate_ptr(isolate) };
    match dispatch::transcode(source, from_encoding, to_encoding) {
        Ok(bytes) => {
            let local: v8::Local<v8::Value> = bytes.to_js(&mut lock);
            // SAFETY: `local` was just created in the isolate's active
            // HandleScope; its FFI representation is handed to the C++
            // caller, which reconstitutes it via `maybe_local_from_ffi` and
            // immediately passes it through `jsg::check()`.
            let raw = unsafe { local.into_ffi() };
            ffi::MaybeLocal { ptr: raw.ptr }
        }
        Err(err) => {
            lock.throw_exception(&err.into());
            ffi::MaybeLocal { ptr: 0 }
        }
    }
}
