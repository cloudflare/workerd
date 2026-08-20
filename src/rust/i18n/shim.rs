// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Thin Rust wrappers around the ICU/simdutf primitives exposed by the C++
//! shim (`shim.h` / `shim.c++`). All the transcoding *logic* -- dispatch,
//! sizing, substitute-character setup, truncation -- lives in [`crate::dispatch`];
//! this module only adapts the shim's C-ish sentinel-value return conventions
//! (`-1` for ICU failure, `0` for simdutf failure) into idiomatic `Option`s.

use crate::error::TranscodeError;
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
///
/// The bridge `Encoding` enum is a `cxx` shared enum, which is a `u8` newtype
/// rather than a real Rust enum, so a value outside the four declared variants
/// is representable. It can only arise if the C++ and Rust halves of the
/// bridge disagree, and is reported as an error rather than a panic because a
/// panic crossing the bridge aborts the process.
fn icu_name(encoding: ffi::Encoding) -> Result<&'static str, TranscodeError> {
    match encoding {
        ffi::Encoding::Ascii => Ok("us-ascii"),
        ffi::Encoding::Latin1 => Ok("iso8859-1"),
        ffi::Encoding::Utf16Le => Ok("utf16le"),
        ffi::Encoding::Utf8 => Ok("utf-8"),
        _ => Err(TranscodeError::InvalidEncoding),
    }
}

impl Converter {
    /// Opens an ICU converter for `encoding`.
    pub fn open(encoding: ffi::Encoding) -> Result<Self, TranscodeError> {
        let conv = ffi::open_converter(icu_name(encoding)?);
        if conv.is_null() {
            return Err(TranscodeError::ConverterOpenFailed);
        }
        Ok(Self(conv))
    }

    /// Returns the largest number of bytes a single character occupies in this
    /// converter's encoding.
    pub fn max_char_size(&self) -> usize {
        self.0.max_char_size()
    }

    /// Returns the smallest number of bytes a single character occupies in this
    /// converter's encoding.
    pub fn min_char_size(&self) -> usize {
        self.0.min_char_size()
    }

    /// Sets the converter's substitute character sequence, used in place of
    /// unmappable characters during conversion.
    pub fn set_subst_chars(&self, substitute: &str) -> Result<(), TranscodeError> {
        if self.0.set_subst_chars(substitute) {
            Ok(())
        } else {
            Err(TranscodeError::SetSubstituteCharsFailed)
        }
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
