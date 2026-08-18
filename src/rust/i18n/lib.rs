// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The engine behind `node:buffer`'s `transcode()`: converts a byte buffer
//! between the four transcodable encodings using ICU and simdutf.
//!
//! The C++ caller owns the destination buffer. It first builds a
//! [`Transcoder`], which validates the input and reports the exact destination
//! size the conversion needs, then allocates a buffer of that size and asks
//! the transcoder to fill it. Separating sizing from writing lets the
//! destination be a JavaScript `Uint8Array`'s backing store, so a conversion
//! writes straight into the buffer that is handed back to JavaScript instead of
//! into an intermediate the caller would have to copy.
//!
//! Nothing here touches V8: the bridge deals only in byte slices, and the
//! caller is responsible for allocating the destination and for narrowing its
//! view of that buffer to the written length.
//!
//! Reached only when the `NODEJS_I18N_RUST` autogate is enabled; otherwise
//! `workerd::api::node::i18n::transcode` (`src/workerd/api/node/i18n.c++`)
//! performs the conversion itself.

mod dispatch;
mod error;
mod shim;

use crate::dispatch::Transcoder;
use crate::error::JsError;

#[cxx::bridge(namespace = "workerd::rust::i18n")]
mod ffi {
    /// The encodings `transcode` supports.
    ///
    /// `src/workerd/api/node/i18n.c++` maps `workerd::api::node::Encoding`
    /// into this type through a `fromImpl` overload, which rejects the
    /// non-transcodable `BASE64`, `BASE64URL`, and `HEX` encodings before any
    /// of this crate runs.
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

    extern "Rust" {
        /// A validated transcode, ready to run. See [`new_transcoder`].
        type Transcoder;

        /// Prepares a transcode of `source` from `from_encoding` to
        /// `to_encoding`.
        ///
        /// Throws if `source` is malformed for `from_encoding`, or if the
        /// destination the conversion would need is too large for an isolate.
        // Boxed because the CXX bridge requires it of opaque Rust types.
        fn new_transcoder(
            source: &[u8],
            from_encoding: Encoding,
            to_encoding: Encoding,
        ) -> Result<Box<Transcoder>>;

        /// The exact size, in bytes, of the destination buffer [`run`]
        /// requires.
        fn dest_len(self: &Transcoder) -> usize;

        /// Transcodes `source` into `dest`, returning the number of bytes
        /// written, which may be less than `dest.len()`.
        ///
        /// `source` must be the same buffer that was passed to
        /// [`new_transcoder`], and `dest` must be exactly [`dest_len`] bytes
        /// long.
        fn run(self: &Transcoder, source: &[u8], dest: &mut [u8]) -> Result<usize>;
    }
}

fn new_transcoder(
    source: &[u8],
    from_encoding: ffi::Encoding,
    to_encoding: ffi::Encoding,
) -> Result<Box<Transcoder>, JsError> {
    Ok(Box::new(Transcoder::new(
        source,
        from_encoding,
        to_encoding,
    )?))
}

impl Transcoder {
    /// The bridge's spelling of [`Transcoder::transcode_into`], reporting
    /// failures as a [`JsError`] so they reach JavaScript as the expected
    /// `Error`.
    fn run(&self, source: &[u8], dest: &mut [u8]) -> Result<usize, JsError> {
        Ok(self.transcode_into(source, dest)?)
    }
}
