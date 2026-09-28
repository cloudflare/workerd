// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust port of `workerd::api::node::i18n::transcode`
//! (`src/workerd/api/node/i18n.c++`), the engine behind `node:buffer`'s
//! `transcode()`. Selected at runtime by the `NODEJS_I18N_RUST` autogate; when
//! the gate is off, the C++ implementation is used instead. The two paths are
//! byte-for-byte and error-message identical: [`dispatch`] ports the C++
//! dispatch/sizing/truncation logic to Rust, while [`codecs`] reimplements, in
//! safe Rust, the ICU conversions and simdutf primitives the C++ path calls.
//! `src/workerd/api/node/i18n-test.c++` checks the two paths against each
//! other.

use jsg::Lock;
use jsg::v8;

mod codecs;
mod dispatch;
mod error;

use crate::dispatch::Transcoder;
use crate::error::TranscodeError;

#[cxx::bridge(namespace = "workerd::rust::i18n")]
mod ffi {
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
    isolate: *mut ffi::Isolate,
    source: &[u8],
    from_encoding: ffi::Encoding,
    to_encoding: ffi::Encoding,
) -> ffi::MaybeLocal {
    // SAFETY: forwarded from this function's own safety contract. The adapter
    // owns the raw-isolate, exception, and local-handle FFI transitions; the
    // feature callback operates only on safe JSG types.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            transcode_impl(lock, source, from_encoding, to_encoding)
        })
    }
}

/// Transcodes `source` into a freshly allocated `Uint8Array`.
///
/// The conversion writes straight into the V8 `ArrayBuffer`'s backing store.
/// There is no intermediate `Vec` and no copy: [`Transcoder`] reports the
/// destination size up front, the buffer is allocated at exactly that size,
/// and the returned view is narrowed to the bytes actually written.
fn transcode_impl<'a>(
    lock: &'a mut Lock,
    source: &[u8],
    from_encoding: ffi::Encoding,
    to_encoding: ffi::Encoding,
) -> jsg::Result<v8::Local<'a, v8::Uint8Array>> {
    let transcoder = Transcoder::new(source, from_encoding, to_encoding)?;
    let (buffer, written) = v8::ArrayBuffer::new_zeroed_with(lock, transcoder.dest_len(), |dest| {
        transcoder.transcode_into(dest)
    })
    .ok_or(TranscodeError::AllocationFailed)?;
    v8::Uint8Array::from_buffer(lock, &buffer, 0, written?)
}

#[cfg(test)]
mod tests {
    use jsg_test::Harness;

    use super::*;

    /// Exercises the full V8 path: allocate, convert into the backing store,
    /// narrow the view. `dispatch.rs` covers conversion behaviour itself; this
    /// checks the parts that only exist once V8 is involved.
    #[test]
    fn transcodes_into_a_narrowed_uint8_array() {
        let harness = Harness::new();
        harness.run_in_context(|lock, _ctx| {
            // '☕' is three UTF-8 bytes and transcodes to the single byte "?"
            // in ASCII, so the destination is allocated at 3 bytes and the
            // returned view must be narrowed to 1.
            let source = "☕".as_bytes();
            let array =
                transcode_impl(lock, source, ffi::Encoding::Utf8, ffi::Encoding::Ascii).unwrap();
            assert_eq!(array.len(), 1);
            assert_eq!(array.as_slice(), b"?");
            Ok(())
        });
    }

    #[test]
    fn empty_input_yields_an_empty_uint8_array() {
        let harness = Harness::new();
        harness.run_in_context(|lock, _ctx| {
            let array =
                transcode_impl(lock, &[], ffi::Encoding::Utf8, ffi::Encoding::Utf16Le).unwrap();
            assert!(array.is_empty());
            Ok(())
        });
    }

    #[test]
    fn full_length_result_is_not_narrowed() {
        let harness = Harness::new();
        harness.run_in_context(|lock, _ctx| {
            // Latin-1 -> UTF-16LE widens every byte to exactly two, so the
            // conversion fills the destination exactly.
            let array =
                transcode_impl(lock, b"Hi", ffi::Encoding::Latin1, ffi::Encoding::Utf16Le).unwrap();
            assert_eq!(array.as_slice(), &[0x48, 0x00, 0x69, 0x00]);
            Ok(())
        });
    }

    #[test]
    fn failure_surfaces_as_an_error() {
        let harness = Harness::new();
        harness.run_in_context(|lock, _ctx| {
            // Odd-length UTF-16LE input is rejected before any allocation.
            let result = transcode_impl(lock, &[0x61], ffi::Encoding::Utf16Le, ffi::Encoding::Utf8);
            assert!(result.is_err());
            Ok(())
        });
    }
}
