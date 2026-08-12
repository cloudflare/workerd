// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Thin Rust wrappers around the ICU/simdutf primitives exposed by the C++
//! shim (`shim.h` / `shim.c++`). All the transcoding *logic* -- dispatch,
//! sizing, substitute-character setup, truncation -- lives in [`crate::dispatch`];
//! this module only adapts the shim's C-ish sentinel-value return conventions
//! (`-1` for ICU failure, `0` for simdutf failure) into idiomatic `Option`s.

use crate::ffi;

/// An open ICU converter for one of the four transcodable encodings.
///
/// Wraps a `cxx::UniquePtr<ffi::Converter>`: the underlying `UConverter*` and
/// its `ucnv_close()` teardown are owned entirely by the C++ shim, so the
/// converter is torn down correctly even if Rust code holding it panics --
/// unlike a raw `UConverter*` smuggled across the FFI boundary, which a panic
/// could leak.
pub struct Converter(cxx::UniquePtr<ffi::Converter>);

/// Returns the ICU converter name for a transcodable encoding, matching
/// `getEncodingName()` in `i18n.c++`.
fn icu_name(encoding: ffi::Encoding) -> &'static str {
    match encoding {
        ffi::Encoding::Ascii => "us-ascii",
        ffi::Encoding::Latin1 => "iso8859-1",
        ffi::Encoding::Utf16Le => "utf16le",
        ffi::Encoding::Utf8 => "utf-8",
        // The bridge `Encoding` enum has exactly these four variants (R4); any
        // other discriminant would mean the C++/Rust enum definitions have
        // drifted out of sync.
        _ => unreachable!("Encoding has exactly four variants"),
    }
}

impl Converter {
    /// Opens an ICU converter for `encoding`.
    ///
    /// Never fails in practice -- the four encoding names above are always
    /// valid ICU converter names -- so a shim-side open failure (see
    /// `shim.c++`) is treated as an unrecoverable invariant violation rather
    /// than a catchable error, matching how unreachable `KJ_ASSERT`-style
    /// conditions are handled elsewhere in this codebase.
    pub fn open(encoding: ffi::Encoding) -> Self {
        Self(ffi::open_converter(icu_name(encoding)))
    }

    pub fn max_char_size(&self) -> usize {
        self.0.max_char_size()
    }

    pub fn min_char_size(&self) -> usize {
        self.0.min_char_size()
    }

    /// Sets the converter's substitute character sequence, used in place of
    /// unmappable characters during conversion.
    pub fn set_subst_chars(&self, substitute: &str) {
        self.0.set_subst_chars(substitute);
    }
}

/// Converts `source` from `from`'s encoding to `to`'s encoding via ICU's
/// `ucnv_convertEx`, mirroring `TranscodeDefault` in `i18n.c++`. Returns the
/// number of bytes written to `target`, or `None` if ICU reports failure.
pub fn convert_ex(
    to: &Converter,
    from: &Converter,
    source: &[u8],
    target: &mut [u8],
) -> Option<usize> {
    usize::try_from(ffi::convert_ex(&to.0, &from.0, source, target)).ok()
}

/// Converts UTF-16LE `source` (as raw bytes) to `to`'s encoding via ICU's
/// `ucnv_fromUChars`, mirroring `TranscodeFromUTF16` in `i18n.c++`. Returns
/// the number of bytes written to `target`, or `None` if ICU reports failure.
pub fn from_uchars(to: &Converter, source: &[u8], target: &mut [u8]) -> Option<usize> {
    usize::try_from(ffi::from_uchars(&to.0, source, target)).ok()
}

/// Widens Latin-1 `source` into UTF-16 (written to `target` as raw bytes),
/// mirroring `simdutf::convert_latin1_to_utf16`. Returns the number of
/// `char16_t` units written.
pub fn convert_latin1_to_utf16(source: &[u8], target: &mut [u8]) -> usize {
    ffi::convert_latin1_to_utf16(source, target)
}

/// Estimates the UTF-16 length (in `char16_t` units) of UTF-8 `source`,
/// mirroring `simdutf::utf16_length_from_utf8`.
pub fn utf16_length_from_utf8(source: &[u8]) -> usize {
    ffi::utf16_length_from_utf8(source)
}

/// Converts UTF-8 `source` to UTF-16LE (written to `target` as raw bytes),
/// mirroring `simdutf::convert_utf8_to_utf16le`. Returns the number of
/// `char16_t` units written, or `0` on invalid UTF-8.
pub fn convert_utf8_to_utf16le(source: &[u8], target: &mut [u8]) -> usize {
    ffi::convert_utf8_to_utf16le(source, target)
}

/// Estimates the UTF-8 length (in bytes) of UTF-16LE `source` (as raw bytes),
/// mirroring `simdutf::utf8_length_from_utf16le`.
pub fn utf8_length_from_utf16le(source: &[u8]) -> usize {
    ffi::utf8_length_from_utf16le(source)
}

/// Converts UTF-16LE `source` (as raw bytes) to UTF-8, mirroring
/// `simdutf::convert_utf16le_to_utf8`. Returns the number of bytes written.
pub fn convert_utf16le_to_utf8(source: &[u8], target: &mut [u8]) -> usize {
    ffi::convert_utf16le_to_utf8(source, target)
}
