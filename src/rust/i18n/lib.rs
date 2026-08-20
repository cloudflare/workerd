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
use jsg::v8;

mod dispatch;
mod error;
mod shim;

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

    unsafe extern "C++" {
        include!("workerd/rust/i18n/shim.h");

        type Converter;

        fn open_converter(name: &str) -> UniquePtr<Converter>;
        fn max_char_size(self: &Converter) -> usize;
        fn min_char_size(self: &Converter) -> usize;
        fn set_subst_chars(self: &Converter, substitute: &str) -> bool;

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
    match transcode_impl(&mut lock, source, from_encoding, to_encoding) {
        Ok(local) => {
            // SAFETY: `local` was just created in the isolate's active
            // HandleScope; its FFI representation is handed to the C++ caller,
            // which reconstitutes it via `maybe_local_from_ffi` and
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

/// Transcodes `source` into a freshly allocated `Uint8Array`.
///
/// The conversion writes straight into the V8 `ArrayBuffer`'s backing store.
/// There is no intermediate `Vec` and no copy: [`Transcoder`] reports the
/// destination size up front, the buffer is allocated at exactly that size,
/// and the returned view is narrowed to the bytes actually written.
fn transcode_impl<'a>(
    lock: &mut Lock,
    source: &[u8],
    from_encoding: ffi::Encoding,
    to_encoding: ffi::Encoding,
) -> Result<v8::Local<'a, v8::Uint8Array>, TranscodeError> {
    let transcoder = Transcoder::new(source, from_encoding, to_encoding)?;

    // Every byte of the buffer is either written by the conversion or excluded
    // from the returned view, so zeroing it first would be wasted work.
    let mut buffer = v8::ArrayBuffer::new_with_mode(
        lock,
        transcoder.dest_len(),
        v8::ffi::BackingStoreInitializationMode::Uninitialized,
    )
    .ok_or(TranscodeError::AllocationFailed)?;

    let written = {
        // SAFETY: `buffer` was created immediately above and has not been
        // handed to JavaScript or aliased by another handle, so this is the
        // only live reference into its backing store. `&mut Lock` is borrowed
        // for the whole of `dest`, so no JavaScript can run and detach the
        // buffer meanwhile.
        let dest = unsafe { buffer.as_mut_slice(lock) };
        transcoder.transcode_into(dest)?
    };

    Ok(v8::Uint8Array::from_buffer(lock, &buffer, 0, written))
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
