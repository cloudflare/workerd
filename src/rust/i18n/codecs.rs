// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Safe wrappers around the ICU and simdutf primitives declared in
//! [`crate::ffi`], which are raw C and C++ entry points taking bare pointers.
//!
//! Everything unsafe about calling them lives here: deriving pointers and
//! lengths from slices, owning the `UConverter`, and turning their C-ish
//! reporting conventions (an out-parameter `UErrorCode`, `-1`, `0`) into
//! `Option` and `Result`. The transcoding *logic* -- dispatch, sizing,
//! substitute-character setup, truncation -- lives in [`crate::dispatch`].

use std::ffi::CStr;
use std::ffi::c_char;

use crate::error::TranscodeError;
use crate::ffi;

/// ICU's `UErrorCode`, a C enum whose underlying type is `int`.
///
/// ICU reports failure through an out parameter of this type rather than a
/// return value, and requires it to be zeroed before each call that is not
/// continuing a previous one.
#[repr(transparent)]
#[derive(Debug, Default, Copy, Clone, PartialEq, Eq)]
pub struct UErrorCode(pub i32);

// SAFETY: layout matches ICU's UErrorCode, a plain C enum over int, which is
// trivially copyable and trivially destructible.
unsafe impl cxx::ExternType for UErrorCode {
    type Id = cxx::type_id!("UErrorCode");
    type Kind = cxx::kind::Trivial;
}

impl UErrorCode {
    /// ICU treats positive codes as failures and negative codes as warnings,
    /// matching the `U_FAILURE` macro.
    fn is_failure(self) -> bool {
        self.0 > 0
    }
}

/// Returns the ICU converter name for a transcodable encoding, matching
/// `getEncodingName()` in `i18n.c++`.
///
/// The bridge `Encoding` enum is a `cxx` shared enum, which is a `u8` newtype
/// rather than a real Rust enum, so a value outside the four declared variants
/// is representable. It can only arise if the C++ and Rust halves of the
/// bridge disagree, and is reported as an error rather than a panic because a
/// panic crossing the bridge aborts the process.
fn icu_name(encoding: ffi::Encoding) -> Result<&'static CStr, TranscodeError> {
    match encoding {
        ffi::Encoding::Ascii => Ok(c"us-ascii"),
        ffi::Encoding::Latin1 => Ok(c"iso8859-1"),
        ffi::Encoding::Utf16Le => Ok(c"utf16le"),
        ffi::Encoding::Utf8 => Ok(c"utf-8"),
        _ => Err(TranscodeError::InvalidEncoding),
    }
}

/// An open ICU converter for one of the four transcodable encodings.
///
/// Owns its `UConverter` and closes it on drop, including while unwinding.
/// Holding a raw pointer makes the type neither `Send` nor `Sync`, which is
/// what we want: ICU converters carry conversion state and are not safe to
/// share between threads.
pub struct Converter(*mut ffi::UConverter);

impl Converter {
    /// Opens an ICU converter for `encoding`.
    pub fn open(encoding: ffi::Encoding) -> Result<Self, TranscodeError> {
        let mut err = UErrorCode::default();
        // SAFETY: `icu_name` returns a NUL-terminated static string, and `err`
        // is a live local for the duration of the call.
        let cnv = unsafe { ffi::ucnv_open(icu_name(encoding)?.as_ptr(), &raw mut err) };
        if err.is_failure() || cnv.is_null() {
            return Err(TranscodeError::ConverterOpenFailed);
        }
        Ok(Self(cnv))
    }

    /// Returns the largest number of bytes a single character occupies in this
    /// converter's encoding.
    pub fn max_char_size(&self) -> usize {
        // SAFETY: `self.0` is non-null for as long as `self` is alive.
        let size = unsafe { ffi::ucnv_getMaxCharSize(self.0) };
        // ICU returns a positive byte count; the cast cannot lose information.
        size.unsigned_abs().into()
    }

    /// Returns the smallest number of bytes a single character occupies in this
    /// converter's encoding.
    pub fn min_char_size(&self) -> usize {
        // SAFETY: `self.0` is non-null for as long as `self` is alive.
        let size = unsafe { ffi::ucnv_getMinCharSize(self.0) };
        size.unsigned_abs().into()
    }

    /// Sets the converter's substitute character sequence, used in place of
    /// unmappable characters during conversion.
    ///
    /// Without this ICU substitutes its own default, which for ASCII is
    /// U+001A rather than the `?` the C++ path produces.
    pub fn set_subst_chars(&self, substitute: &str) -> Result<(), TranscodeError> {
        if substitute.is_empty() {
            return Ok(());
        }
        // ICU takes the length as an `int8_t` and reads a negative length as
        // "NUL-terminated", which `substitute` is not. Its own limit on
        // substitute sequences is far lower still.
        let length =
            i8::try_from(substitute.len()).map_err(|_| TranscodeError::SetSubstituteCharsFailed)?;

        let mut err = UErrorCode::default();
        // SAFETY: `self.0` is non-null, and `substitute` outlives the call and
        // is at least `length` bytes long. ICU takes the sequence as bytes and
        // does not require NUL termination when given an explicit length.
        unsafe {
            ffi::ucnv_setSubstChars(self.0, substitute.as_ptr().cast(), length, &raw mut err);
        }
        if err.is_failure() {
            return Err(TranscodeError::SetSubstituteCharsFailed);
        }
        Ok(())
    }
}

impl Drop for Converter {
    fn drop(&mut self) {
        // SAFETY: `self.0` was returned non-null by `ucnv_open` and is closed
        // exactly once, here.
        unsafe { ffi::ucnv_close(self.0) }
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
    let target_start: *mut c_char = target.as_mut_ptr().cast();
    let mut target_cursor = target_start;
    let mut source_cursor: *const c_char = source.as_ptr().cast();
    let mut err = UErrorCode::default();

    // SAFETY: both cursors start at the base of a live slice and are bounded
    // by a limit one past that slice's end, which is what ICU advances them
    // against. Passing a null pivot asks ICU to use an internal one.
    unsafe {
        ffi::ucnv_convertEx(
            to.0,
            from.0,
            &raw mut target_cursor,
            target_start.add(target.len()),
            &raw mut source_cursor,
            source_cursor.add(source.len()),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null(),
            1, // reset
            1, // flush
            &raw mut err,
        );
    }
    if err.is_failure() {
        return None;
    }
    // SAFETY: ICU advanced `target_cursor` within `target`, so both pointers
    // are into the same allocation.
    let written = unsafe { target_cursor.offset_from(target_start) };
    usize::try_from(written).ok()
}

/// Converts UTF-16LE `source` (as raw bytes) to `to`'s encoding via ICU's
/// `ucnv_fromUChars`, mirroring `TranscodeFromUTF16` in `i18n.c++`. Returns
/// the number of bytes written to `target`, or `None` if ICU reports failure.
pub fn from_uchars(to: &Converter, source: &[u8], target: &mut [u8]) -> Option<usize> {
    let src_length = i32::try_from(source.len() / size_of::<u16>()).ok()?;
    let dest_capacity = i32::try_from(target.len()).ok()?;
    let mut err = UErrorCode::default();

    // SAFETY: the pointers and lengths describe the two live slices. `source`
    // need not be `u16`-aligned -- it is caller-supplied buffer contents, which
    // a Uint8Array can expose at an odd byteOffset -- and is reinterpreted as
    // `UChar*` exactly as `i18n.c++` does with the same bytes. Casting a raw
    // pointer is well-defined in Rust regardless of alignment; no reference to
    // the misaligned data is ever formed on this side.
    let len = unsafe {
        ffi::ucnv_fromUChars(
            to.0,
            target.as_mut_ptr().cast(),
            dest_capacity,
            source.as_ptr().cast(),
            src_length,
            &raw mut err,
        )
    };
    if err.is_failure() {
        return None;
    }
    usize::try_from(len).ok()
}

/// Widens Latin-1 `source` into UTF-16 (written to `target` as raw bytes),
/// mirroring `simdutf::convert_latin1_to_utf16`. Returns the number of
/// `char16_t` units written.
pub fn convert_latin1_to_utf16(source: &[u8], target: &mut [u8]) -> usize {
    // SAFETY: `target` holds at least `source.len()` UTF-16 units, which
    // `dispatch` guarantees by sizing it at two bytes per source byte. See
    // `from_uchars` on the alignment of these casts.
    unsafe {
        ffi::convert_latin1_to_utf16(
            source.as_ptr().cast(),
            source.len(),
            target.as_mut_ptr().cast(),
        )
    }
}

/// Estimates the UTF-16 length (in `char16_t` units) of UTF-8 `source`,
/// mirroring `simdutf::utf16_length_from_utf8`.
pub fn utf16_length_from_utf8(source: &[u8]) -> usize {
    // SAFETY: pointer and length describe a live slice.
    unsafe { ffi::utf16_length_from_utf8(source.as_ptr().cast(), source.len()) }
}

/// Converts UTF-8 `source` to UTF-16LE (written to `target` as raw bytes),
/// mirroring `simdutf::convert_utf8_to_utf16le`. Returns the number of
/// `char16_t` units written, or `0` on invalid UTF-8.
pub fn convert_utf8_to_utf16le(source: &[u8], target: &mut [u8]) -> usize {
    // SAFETY: `target` is sized by `dispatch` at two bytes per unit that
    // `utf16_length_from_utf8` reported for this same `source`, which is the
    // most this call can write.
    unsafe {
        ffi::convert_utf8_to_utf16le(
            source.as_ptr().cast(),
            source.len(),
            target.as_mut_ptr().cast(),
        )
    }
}

/// Estimates the UTF-8 length (in bytes) of UTF-16LE `source` (as raw bytes),
/// mirroring `simdutf::utf8_length_from_utf16le`.
pub fn utf8_length_from_utf16le(source: &[u8]) -> usize {
    // SAFETY: pointer and length describe a live slice.
    unsafe {
        ffi::utf8_length_from_utf16le(source.as_ptr().cast(), source.len() / size_of::<u16>())
    }
}

/// Converts UTF-16LE `source` (as raw bytes) to UTF-8, mirroring
/// `simdutf::convert_utf16le_to_utf8`. Returns the number of bytes written.
pub fn convert_utf16le_to_utf8(source: &[u8], target: &mut [u8]) -> usize {
    // SAFETY: `target` is sized by `dispatch` at the byte count
    // `utf8_length_from_utf16le` reported for this same `source`, which is the
    // most this call can write.
    unsafe {
        ffi::convert_utf16le_to_utf8(
            source.as_ptr().cast(),
            source.len() / size_of::<u16>(),
            target.as_mut_ptr().cast(),
        )
    }
}
