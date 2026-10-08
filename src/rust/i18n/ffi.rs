// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The FFI island of the crate: the `#[cxx::bridge]` and the one hand-written
//! `unsafe`, the entry point that turns C++'s raw `v8::Isolate*` into a
//! [`jsg::Lock`]. No other module needs `unsafe`.
#![allow(
    unsafe_code,
    reason = "the crate's FFI island: the cxx bridge and the raw-isolate entry point"
)]

pub use bridge::Encoding;
use jsg::v8;

#[cxx::bridge(namespace = "workerd::rust::i18n")]
mod bridge {
    /// The four encodings `i18n::transcode` supports. Mirrors the
    /// transcodable subset of `workerd::api::node::Encoding`
    /// (`src/workerd/api/node/i18n.h`). `src/workerd/api/node/i18n.c++` maps
    /// into this type through a `fromImpl` overload that rejects the
    /// non-transcodable `BASE64`, `BASE64URL`, and `HEX` variants before ever
    /// calling into Rust.
    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    #[repr(u8)]
    enum Encoding {
        Ascii,
        Latin1,
        Utf8,
        Utf16Le,
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
    isolate: *mut bridge::Isolate,
    source: &[u8],
    from_encoding: Encoding,
    to_encoding: Encoding,
) -> bridge::MaybeLocal {
    // SAFETY: forwarded from this function's own safety contract. The adapter
    // owns the raw-isolate, exception, and local-handle FFI transitions; the
    // feature callback operates only on safe JSG types.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            crate::transcode_impl(lock, source, from_encoding, to_encoding)
        })
    }
}
