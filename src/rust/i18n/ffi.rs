// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The FFI island of the crate, and the only module that needs `unsafe`. It
//! holds the `#[cxx::bridge]`; the entry point that turns C++'s raw
//! `v8::Isolate*` into a [`jsg::Lock`]; and safe wrappers around the ICU
//! converter calls and simdutf calls `i18n.c++` makes, through `rust_icu_sys`
//! and `ffi.h`. Both run in the libraries workerd links, so every conversion is
//! the same code the C++ path runs.
#![allow(
    unsafe_code,
    reason = "the crate's FFI island: the cxx bridge, the raw-isolate entry point, and the ICU and simdutf calls"
)]

use std::ffi::CStr;
use std::ffi::c_char;

pub use bridge::Encoding;
use jsg::v8;
use rust_icu_sys as sys;
use rust_icu_sys::versioned_function;

use crate::error::TranscodeError;

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

    unsafe extern "C++" {
        include!("workerd/rust/i18n/ffi.h");

        unsafe fn simdutf_convert_latin1_to_utf16(
            input: *const u8,
            length: usize,
            utf16_output: *mut u16,
        ) -> usize;
        unsafe fn simdutf_utf16_length_from_utf8(input: *const u8, length: usize) -> usize;
        unsafe fn simdutf_convert_utf8_to_utf16le(
            input: *const u8,
            length: usize,
            utf16_output: *mut u16,
        ) -> usize;
        unsafe fn simdutf_utf8_length_from_utf16le(input: *const u16, length: usize) -> usize;
        unsafe fn simdutf_convert_utf16le_to_utf8(
            input: *const u16,
            length: usize,
            utf8_output: *mut u8,
        ) -> usize;
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

// ICU
//
// Everything unsafe about calling ICU: deriving pointers and lengths from
// slices, owning the `UConverter`, and turning ICU's out-parameter
// `UErrorCode` into `Option` and `Result`.

/// ICU treats positive codes as failures and negative codes as warnings,
/// matching the `U_FAILURE` macro.
fn is_failure(error: sys::UErrorCode) -> bool {
    error > sys::UErrorCode::U_ZERO_ERROR
}

/// The ICU converter name for `encoding`, matching `getEncodingName()` in
/// `i18n.c++`.
///
/// The bridge `Encoding` is a `cxx` shared enum, and so a `u8` newtype rather
/// than a real Rust enum. As in `i18n.c++`, whose `getEncodingName` ends in
/// `default: KJ_UNREACHABLE`, an unknown encoding is unreachable here:
/// `dispatch` rejects an unknown source encoding up front, the C++ `fromImpl`
/// conversion rejects anything but the four transcodable encodings before
/// Rust is entered, and a panic in Rust reaches C++ as a `kj::Exception`.
fn icu_name(encoding: Encoding) -> &'static CStr {
    match encoding {
        Encoding::Ascii => c"us-ascii",
        Encoding::Latin1 => c"iso8859-1",
        Encoding::Utf8 => c"utf-8",
        Encoding::Utf16Le => c"utf16le",
        _ => unreachable!("invalid encoding {encoding:?}"),
    }
}

/// An open ICU converter, mirroring `i18n::Converter` in `i18n.c++`.
///
/// Owns its `UConverter` and closes it on drop, including while unwinding.
/// Holding a raw pointer makes the type neither `Send` nor `Sync`, which is
/// what we want: ICU converters carry conversion state and are not safe to
/// share between threads.
pub struct Converter {
    cnv: *mut sys::UConverter,
}

impl Converter {
    /// Opens an ICU converter for `encoding`.
    pub fn open(encoding: Encoding) -> Result<Self, TranscodeError> {
        let mut err = sys::UErrorCode::U_ZERO_ERROR;
        // SAFETY: the name is a NUL-terminated static string, and `err` is a
        // live local for the duration of the call.
        let cnv =
            unsafe { versioned_function!(ucnv_open)(icu_name(encoding).as_ptr(), &raw mut err) };
        if is_failure(err) || cnv.is_null() {
            return Err(TranscodeError::ConverterOpenFailed);
        }
        Ok(Self { cnv })
    }

    /// The most bytes one UTF-16 code unit can occupy in this converter's
    /// encoding, from `ucnv_getMaxCharSize`.
    pub fn max_char_size(&self) -> usize {
        // SAFETY: `self.cnv` is a live converter returned by `ucnv_open`.
        let size = unsafe { versioned_function!(ucnv_getMaxCharSize)(self.cnv) };
        // ICU reports a small positive `int8_t`.
        usize::from(size.unsigned_abs())
    }

    /// The fewest bytes one character can occupy in this converter's
    /// encoding, from `ucnv_getMinCharSize`.
    pub fn min_char_size(&self) -> usize {
        // SAFETY: `self.cnv` is a live converter returned by `ucnv_open`.
        let size = unsafe { versioned_function!(ucnv_getMinCharSize)(self.cnv) };
        // ICU reports a small positive `int8_t`.
        usize::from(size.unsigned_abs())
    }

    /// Sets the converter's substitute character sequence, used in place of
    /// unmappable characters during conversion. An empty `substitute` leaves
    /// ICU's default in place, as in `i18n.c++`.
    pub fn set_subst_chars(&self, substitute: &str) -> Result<(), TranscodeError> {
        if substitute.is_empty() {
            return Ok(());
        }
        // ICU takes the length as an `int8_t` and reads a negative length as
        // "NUL-terminated", which `substitute` is not. Its own limit on
        // substitute sequences is far lower still.
        let length =
            i8::try_from(substitute.len()).map_err(|_| TranscodeError::SetSubstituteCharsFailed)?;

        let mut err = sys::UErrorCode::U_ZERO_ERROR;
        // SAFETY: `self.cnv` is a live converter, and `substitute` outlives the
        // call and is `length` bytes long. ICU takes the sequence as bytes and
        // does not require NUL termination when given an explicit length.
        unsafe {
            versioned_function!(ucnv_setSubstChars)(
                self.cnv,
                substitute.as_ptr().cast(),
                length,
                &raw mut err,
            );
        }
        if is_failure(err) {
            return Err(TranscodeError::SetSubstituteCharsFailed);
        }
        Ok(())
    }
}

impl Drop for Converter {
    fn drop(&mut self) {
        // SAFETY: `self.cnv` was returned non-null by `ucnv_open` and is closed
        // exactly once, here.
        unsafe { versioned_function!(ucnv_close)(self.cnv) }
    }
}

/// Converts `source` from `from`'s encoding to `to`'s via ICU's
/// `ucnv_convertEx`, as `TranscodeDefault` in `i18n.c++` does. Returns the
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
    let mut err = sys::UErrorCode::U_ZERO_ERROR;

    // SAFETY: both cursors start at the base of a live slice and are bounded
    // by a limit one past that slice's end, which is what ICU advances them
    // against. Passing a null pivot asks ICU to use an internal one.
    unsafe {
        versioned_function!(ucnv_convertEx)(
            to.cnv,
            from.cnv,
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
    if is_failure(err) {
        return None;
    }
    // SAFETY: ICU advanced `target_cursor` within `target`, so both pointers
    // are into the same allocation.
    let written = unsafe { target_cursor.offset_from(target_start) };
    usize::try_from(written).ok()
}

/// Converts UTF-16LE `source` (as raw bytes) to `to`'s encoding via ICU's
/// `ucnv_fromUChars`, as `TranscodeFromUTF16` in `i18n.c++` does. A trailing
/// odd byte is ignored. Returns the number of bytes written to `target`, or
/// `None` if ICU reports failure.
pub fn from_uchars(to: &Converter, source: &[u8], target: &mut [u8]) -> Option<usize> {
    let src_length = i32::try_from(source.len() / size_of::<u16>()).ok()?;
    let dest_capacity = i32::try_from(target.len()).ok()?;
    let mut err = sys::UErrorCode::U_ZERO_ERROR;

    // SAFETY: the pointers and lengths describe the two live slices. `source`
    // need not be `u16`-aligned -- it is caller-supplied buffer contents, which
    // a Uint8Array can expose at an odd byteOffset -- and is reinterpreted as
    // `UChar*` exactly as `i18n.c++` does with the same bytes. Casting a raw
    // pointer is well-defined in Rust regardless of alignment; no reference to
    // the misaligned data is ever formed on this side.
    let len = unsafe {
        versioned_function!(ucnv_fromUChars)(
            to.cnv,
            target.as_mut_ptr().cast(),
            dest_capacity,
            source.as_ptr().cast(),
            src_length,
            &raw mut err,
        )
    };
    if is_failure(err) {
        return None;
    }
    usize::try_from(len).ok()
}

// simdutf
//
// The simdutf C++ functions `i18n.c++` calls, through the inline forwarders in
// `ffi.h`, declared in the bridge above.
//
// simdutf does not bound its writes by a destination length. Its conversions
// write at most the length its own estimate reports for the same input, valid
// or not: its SIMD kernels stop short of the end of the input by a margin
// that keeps their wide stores inside an estimate-sized buffer ("to avoid
// overruns", in `utf8_to_utf16::validating_transcoder::convert` and
// `avx2_convert_utf16_to_utf8`), and report errors only after converting. The
// C++ path relies on this too. [`Utf8Source`] and [`Utf16LeSource`] hold each
// source together with the estimate taken from it, so a conversion can check
// its destination against that estimate.
//
// UTF-16 is passed to simdutf as raw bytes reinterpreted as `char16_t*`,
// exactly as `i18n.c++` passes the same bytes, and so need not be
// `u16`-aligned; see [`from_uchars`].

/// Widens Latin-1 `source` into UTF-16LE (written to `target` as raw bytes)
/// via `simdutf::convert_latin1_to_utf16`, as `TranscodeLatin1ToUTF16` in
/// `i18n.c++` does. Returns the number of UTF-16 code units written, or `None`
/// if `target` cannot hold two bytes per source byte.
pub fn convert_latin1_to_utf16(source: &[u8], target: &mut [u8]) -> Option<usize> {
    if target.len() / 2 < source.len() {
        return None;
    }
    // SAFETY: every Latin-1 byte widens to exactly one code unit, and `target`
    // holds at least `source.len()` of them.
    Some(unsafe {
        bridge::simdutf_convert_latin1_to_utf16(
            source.as_ptr().cast(),
            source.len(),
            target.as_mut_ptr().cast(),
        )
    })
}

/// UTF-8 bytes measured by `simdutf::utf16_length_from_utf8`.
pub struct Utf8Source<'a> {
    bytes: &'a [u8],
    utf16_len: usize,
}

impl<'a> Utf8Source<'a> {
    pub fn new(bytes: &'a [u8]) -> Self {
        // SAFETY: pointer and length describe a live slice.
        let utf16_len =
            unsafe { bridge::simdutf_utf16_length_from_utf8(bytes.as_ptr().cast(), bytes.len()) };
        Self { bytes, utf16_len }
    }

    /// simdutf's estimate of the UTF-16 length, in code units. Exact for
    /// valid UTF-8; for invalid UTF-8 it is only the count simdutf reports.
    pub const fn utf16_len(&self) -> usize {
        self.utf16_len
    }

    /// Converts to UTF-16LE (written to `target` as raw bytes) via
    /// `simdutf::convert_utf8_to_utf16le`, as `TranscodeUTF16FromUTF8` in
    /// `i18n.c++` does. Returns the number of code units written, which is `0`
    /// for invalid UTF-8, or `None` if `target` cannot hold
    /// [`Utf8Source::utf16_len`] code units.
    pub fn convert_to_utf16le(&self, target: &mut [u8]) -> Option<usize> {
        if target.len() / 2 < self.utf16_len {
            return None;
        }
        // SAFETY: `target` holds the `utf16_len` code units simdutf estimated
        // for these same bytes, which is the most it writes for them; see the
        // section comment above.
        Some(unsafe {
            bridge::simdutf_convert_utf8_to_utf16le(
                self.bytes.as_ptr().cast(),
                self.bytes.len(),
                target.as_mut_ptr().cast(),
            )
        })
    }
}

/// UTF-16LE code units (as raw bytes) measured by
/// `simdutf::utf8_length_from_utf16le`. A trailing odd byte is ignored.
pub struct Utf16LeSource<'a> {
    bytes: &'a [u8],
    utf8_len: usize,
}

impl<'a> Utf16LeSource<'a> {
    pub fn new(bytes: &'a [u8]) -> Self {
        // SAFETY: pointer and length describe the whole code units of a live
        // slice.
        let utf8_len = unsafe {
            bridge::simdutf_utf8_length_from_utf16le(
                bytes.as_ptr().cast(),
                bytes.len() / size_of::<u16>(),
            )
        };
        Self { bytes, utf8_len }
    }

    /// simdutf's estimate of the UTF-8 length, in bytes. Exact for valid
    /// UTF-16. An unpaired surrogate has no UTF-8 encoding, and what it counts
    /// for depends on the kernel simdutf selects for the CPU: two bytes, or
    /// three (the size of U+FFFD) in the AVX-512 kernel's path for inputs of
    /// up to 32 code units.
    pub const fn utf8_len(&self) -> usize {
        self.utf8_len
    }

    /// Converts to UTF-8 via `simdutf::convert_utf16le_to_utf8`, as
    /// `TranscodeUTF8FromUTF16` in `i18n.c++` does. Returns the number of
    /// bytes written, which is `0` if the source contains an unpaired
    /// surrogate, or `None` if `target` cannot hold
    /// [`Utf16LeSource::utf8_len`] bytes.
    pub fn convert_to_utf8(&self, target: &mut [u8]) -> Option<usize> {
        if target.len() < self.utf8_len {
            return None;
        }
        // SAFETY: `target` holds the `utf8_len` bytes simdutf estimated for
        // these same code units, which is the most it writes for them; see
        // the section comment above.
        Some(unsafe {
            bridge::simdutf_convert_utf16le_to_utf8(
                self.bytes.as_ptr().cast(),
                self.bytes.len() / size_of::<u16>(),
                target.as_mut_ptr().cast(),
            )
        })
    }
}

#[cfg(test)]
#[path = "ffi-test.rs"]
mod tests;
