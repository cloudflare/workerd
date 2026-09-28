// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The codec primitives behind [`crate::dispatch`]: safe wrappers around the
//! ICU converters exposed by `rust_icu_sys`, and safe Rust ports of the
//! simdutf functions `i18n.c++` calls.
//!
//! Everything unsafe about calling ICU lives here: deriving pointers and
//! lengths from slices, owning the `UConverter`, and turning ICU's
//! out-parameter `UErrorCode` into `Option` and `Result`. The simdutf ports
//! need no `unsafe` at all. The transcoding *logic* -- dispatch, sizing,
//! substitute-character setup, truncation -- lives in [`crate::dispatch`].

use std::ffi::CStr;
use std::ffi::c_char;

use rust_icu_sys as sys;
use rust_icu_sys::versioned_function;

use crate::error::TranscodeError;
use crate::ffi;

/// ICU treats positive codes as failures and negative codes as warnings,
/// matching the `U_FAILURE` macro.
fn is_failure(error: sys::UErrorCode) -> bool {
    error > sys::UErrorCode::U_ZERO_ERROR
}

/// The fixed properties of the ICU converter for one transcodable encoding.
struct EncodingInfo {
    /// The ICU converter name, matching `getEncodingName()` in `i18n.c++`.
    icu_name: &'static CStr,
    /// What `ucnv_getMinCharSize` returns for this converter.
    min_char_size: usize,
    /// What `ucnv_getMaxCharSize` returns for this converter.
    max_char_size: usize,
}

// The char sizes are ICU's `minBytesPerChar` / `maxBytesPerChar`, which ICU
// hard-codes in each algorithmic converter's `UConverterStaticData`
// (`ucnvlat1.cpp`, `ucnv_u8.cpp`, `ucnv_u16.cpp`) and which
// `ucnv_getMinCharSize` / `ucnv_getMaxCharSize` return unchanged. ICU measures
// them per UTF-16 code unit, which is why UTF-8's maximum is 3 rather than 4:
// a supplementary character is two code units and four bytes.
const ASCII: EncodingInfo = EncodingInfo {
    icu_name: c"us-ascii",
    min_char_size: 1,
    max_char_size: 1,
};
const LATIN1: EncodingInfo = EncodingInfo {
    icu_name: c"iso8859-1",
    min_char_size: 1,
    max_char_size: 1,
};
const UTF8: EncodingInfo = EncodingInfo {
    icu_name: c"utf-8",
    min_char_size: 1,
    max_char_size: 3,
};
const UTF16LE: EncodingInfo = EncodingInfo {
    icu_name: c"utf16le",
    min_char_size: 2,
    max_char_size: 2,
};

/// Returns the converter properties of a transcodable encoding.
///
/// The bridge `Encoding` enum is a `cxx` shared enum, which is a `u8` newtype
/// rather than a real Rust enum, so a value outside the four declared variants
/// is representable. It can only arise if the C++ and Rust halves of the
/// bridge disagree, and is reported as an error rather than a panic because a
/// panic crossing the bridge aborts the process.
fn encoding_info(encoding: ffi::Encoding) -> Result<&'static EncodingInfo, TranscodeError> {
    match encoding {
        ffi::Encoding::Ascii => Ok(&ASCII),
        ffi::Encoding::Latin1 => Ok(&LATIN1),
        ffi::Encoding::Utf8 => Ok(&UTF8),
        ffi::Encoding::Utf16Le => Ok(&UTF16LE),
        _ => Err(TranscodeError::InvalidEncoding),
    }
}

/// An open ICU converter for one of the four transcodable encodings.
///
/// Owns its `UConverter` and closes it on drop, including while unwinding.
/// Holding a raw pointer makes the type neither `Send` nor `Sync`, which is
/// what we want: ICU converters carry conversion state and are not safe to
/// share between threads.
pub struct Converter {
    cnv: *mut sys::UConverter,
    info: &'static EncodingInfo,
}

impl Converter {
    /// Opens an ICU converter for `encoding`.
    pub fn open(encoding: ffi::Encoding) -> Result<Self, TranscodeError> {
        let info = encoding_info(encoding)?;
        let mut err = sys::UErrorCode::U_ZERO_ERROR;
        // SAFETY: `icu_name` is a NUL-terminated static string, and `err` is a
        // live local for the duration of the call.
        let cnv = unsafe { versioned_function!(ucnv_open)(info.icu_name.as_ptr(), &raw mut err) };
        if is_failure(err) || cnv.is_null() {
            return Err(TranscodeError::ConverterOpenFailed);
        }
        Ok(Self { cnv, info })
    }

    /// Returns the largest number of bytes a single UTF-16 code unit occupies
    /// in this converter's encoding, as `ucnv_getMaxCharSize` reports it.
    pub const fn max_char_size(&self) -> usize {
        self.info.max_char_size
    }

    /// Returns the smallest number of bytes a single character occupies in
    /// this converter's encoding, as `ucnv_getMinCharSize` reports it.
    pub const fn min_char_size(&self) -> usize {
        self.info.min_char_size
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

        let mut err = sys::UErrorCode::U_ZERO_ERROR;
        // SAFETY: `self.cnv` is non-null, and `substitute` outlives the call and
        // is at least `length` bytes long. ICU takes the sequence as bytes and
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
/// `ucnv_fromUChars`, mirroring `TranscodeFromUTF16` in `i18n.c++`. Returns
/// the number of bytes written to `target`, or `None` if ICU reports failure.
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

// simdutf ports
//
// The functions below reproduce the simdutf calls `i18n.c++` makes, as safe
// Rust. The two length estimates compute the same per-byte and per-unit
// counts as simdutf's scalar reference implementations
// (`simdutf::scalar::utf8::utf16_length_from_utf8` and
// `simdutf::scalar::utf16::utf8_length_from_utf16`), whose results every SIMD
// kernel reproduces exactly; the estimates are not validated, so their exact
// values on malformed input matter to `dispatch`. The conversions validate
// their input with the same Unicode rules simdutf applies, so they accept and
// reject exactly the same inputs and produce the same bytes.
//
// UTF-16 is read and written as little-endian byte pairs, so neither side
// needs to be `u16`-aligned. A `target` shorter than the conversion needs is
// not undefined behaviour here as it is in simdutf: the conversion stops at
// the end of `target` and reports what it wrote.

/// Widens Latin-1 `source` into UTF-16LE (written to `target` as raw bytes),
/// mirroring `simdutf::convert_latin1_to_utf16`. Returns the number of UTF-16
/// code units written.
pub fn convert_latin1_to_utf16(source: &[u8], target: &mut [u8]) -> usize {
    let (units, _) = target.as_chunks_mut::<2>();
    let mut written = 0;
    for (&byte, unit) in source.iter().zip(units) {
        *unit = u16::from(byte).to_le_bytes();
        written += 1;
    }
    written
}

/// The number of lanes the two length estimates count in parallel.
///
/// Each estimate sums a small per-element count into an array of narrow
/// counters, one per lane, which LLVM vectorizes into the same packed
/// compare-and-subtract loop simdutf's kernels use. Flushing the counters into
/// a `usize` only every so many blocks is what keeps them narrow: widening
/// every element straight to `usize` vectorizes too, but several times slower.
/// 64 lanes measured fastest for both estimates at the production x86-64
/// target features.
const LENGTH_LANES: usize = 64;

/// The UTF-16 code units UTF-8 byte `byte` contributes to
/// [`utf16_length_from_utf8`]: one unless it is a continuation byte, plus one
/// more for a four-byte lead byte, which encodes a surrogate pair. At most 2.
fn utf16_units_from_utf8_byte(byte: u8) -> u8 {
    u8::from(byte & 0xc0 != 0x80) + u8::from(byte >= 0xf0)
}

/// Estimates the UTF-16 length (in code units) of UTF-8 `source`, mirroring
/// `simdutf::utf16_length_from_utf8`.
///
/// Exact for valid UTF-8; for invalid UTF-8 it is only the count simdutf would
/// report.
pub fn utf16_length_from_utf8(source: &[u8]) -> usize {
    // A lane gains at most 2 per block, so a `u8` counter holds 127 blocks.
    const BLOCKS_PER_FLUSH: usize = 127;

    let (blocks, tail) = source.as_chunks::<LENGTH_LANES>();
    let mut total = 0;
    for group in blocks.chunks(BLOCKS_PER_FLUSH) {
        let mut counts = [0u8; LENGTH_LANES];
        for block in group {
            for (count, &byte) in counts.iter_mut().zip(block) {
                *count += utf16_units_from_utf8_byte(byte);
            }
        }
        total += counts
            .iter()
            .map(|&count| usize::from(count))
            .sum::<usize>();
    }
    total
        + tail
            .iter()
            .map(|&byte| usize::from(utf16_units_from_utf8_byte(byte)))
            .sum::<usize>()
}

/// Converts UTF-8 `source` to UTF-16LE (written to `target` as raw bytes),
/// mirroring `simdutf::convert_utf8_to_utf16le`. Returns the number of UTF-16
/// code units written, or `0` on invalid UTF-8.
pub fn convert_utf8_to_utf16le(source: &[u8], target: &mut [u8]) -> usize {
    let Ok(text) = std::str::from_utf8(source) else {
        return 0;
    };
    let (units, _) = target.as_chunks_mut::<2>();
    let mut written = 0;
    for (code_unit, unit) in text.encode_utf16().zip(units) {
        *unit = code_unit.to_le_bytes();
        written += 1;
    }
    written
}

/// The UTF-8 bytes UTF-16 code unit `unit` contributes to
/// [`utf8_length_from_utf16le`]: one, a second above U+007F, and a third above
/// U+07FF unless it is a surrogate. A surrogate thus counts two bytes, so a
/// valid pair counts the four its character encodes to; an unpaired surrogate
/// also counts two, though it has no UTF-8 encoding. Between 1 and 3.
fn utf8_bytes_from_utf16_unit(unit: u16) -> u16 {
    // Cannot underflow: a surrogate is above U+07FF.
    1 + u16::from(unit > 0x7f) + u16::from(unit > 0x7ff) - u16::from(unit & 0xf800 == 0xd800)
}

/// Estimates the UTF-8 length (in bytes) of UTF-16LE `source` (as raw bytes),
/// mirroring `simdutf::utf8_length_from_utf16le`. A trailing odd byte is
/// ignored.
pub fn utf8_length_from_utf16le(source: &[u8]) -> usize {
    // A lane gains at most 3 per block, so a `u16` counter holds 21845 blocks.
    const BLOCKS_PER_FLUSH: usize = 21845;

    let (units, _) = source.as_chunks::<2>();
    let (blocks, tail) = units.as_chunks::<LENGTH_LANES>();
    let mut total = 0;
    for group in blocks.chunks(BLOCKS_PER_FLUSH) {
        let mut counts = [0u16; LENGTH_LANES];
        for block in group {
            for (count, &unit) in counts.iter_mut().zip(block) {
                *count += utf8_bytes_from_utf16_unit(u16::from_le_bytes(unit));
            }
        }
        total += counts
            .iter()
            .map(|&count| usize::from(count))
            .sum::<usize>();
    }
    total
        + tail
            .iter()
            .map(|&unit| usize::from(utf8_bytes_from_utf16_unit(u16::from_le_bytes(unit))))
            .sum::<usize>()
}

/// Converts UTF-16LE `source` (as raw bytes) to UTF-8, mirroring
/// `simdutf::convert_utf16le_to_utf8`. Returns the number of bytes written, or
/// `0` if `source` contains an unpaired surrogate. A trailing odd byte is
/// ignored.
pub fn convert_utf16le_to_utf8(source: &[u8], target: &mut [u8]) -> usize {
    let (units, _) = source.as_chunks::<2>();
    let mut written = 0;
    for ch in char::decode_utf16(units.iter().map(|&unit| u16::from_le_bytes(unit))) {
        let Ok(ch) = ch else {
            return 0;
        };
        let Some(out) = target.get_mut(written..written + ch.len_utf8()) else {
            break;
        };
        written += ch.encode_utf8(out).len();
    }
    written
}

#[cfg(test)]
mod tests {
    use jsg_test::Harness;

    use super::*;

    const ENCODINGS: [ffi::Encoding; 4] = [
        ffi::Encoding::Ascii,
        ffi::Encoding::Latin1,
        ffi::Encoding::Utf8,
        ffi::Encoding::Utf16Le,
    ];

    fn utf16le(text: &str) -> Vec<u8> {
        text.encode_utf16().flat_map(u16::to_le_bytes).collect()
    }

    /// The char sizes are copied out of ICU's converter tables; check them
    /// against the converters themselves so an ICU change cannot silently
    /// desynchronize them.
    #[test]
    fn char_sizes_match_icu() {
        // Installs the embedded ICU data, without which converters fail to open.
        let _harness = Harness::new();
        for encoding in ENCODINGS {
            let converter = Converter::open(encoding).unwrap();
            // SAFETY: `converter.cnv` is a live converter returned by `ucnv_open`.
            let (min, max) = unsafe {
                (
                    versioned_function!(ucnv_getMinCharSize)(converter.cnv),
                    versioned_function!(ucnv_getMaxCharSize)(converter.cnv),
                )
            };
            assert_eq!(
                converter.min_char_size(),
                usize::try_from(min).unwrap(),
                "{encoding:?} min"
            );
            assert_eq!(
                converter.max_char_size(),
                usize::try_from(max).unwrap(),
                "{encoding:?} max"
            );
        }
    }

    #[test]
    fn latin1_widens_every_byte() {
        let source: Vec<u8> = (0..=255).collect();
        let mut target = vec![0; source.len() * 2];
        assert_eq!(convert_latin1_to_utf16(&source, &mut target), 256);
        let expected: Vec<u8> = (0..=255u16).flat_map(u16::to_le_bytes).collect();
        assert_eq!(target, expected);
    }

    #[test]
    fn utf16_length_from_utf8_matches_simdutf() {
        // Exact on valid input: 1 + 1 + 1 + 2 units.
        assert_eq!(utf16_length_from_utf8("aé☕😀".as_bytes()), 5);
        // Continuation bytes alone count nothing.
        assert_eq!(utf16_length_from_utf8(&[0x80, 0xbf]), 0);
        // Unvalidated: a lone four-byte lead byte still counts two, and bytes
        // that are never valid UTF-8 count as lead bytes.
        assert_eq!(utf16_length_from_utf8(&[0xf0]), 2);
        assert_eq!(utf16_length_from_utf8(&[0xc0, 0xff]), 3);
    }

    #[test]
    fn utf8_length_from_utf16le_matches_simdutf() {
        // Exact on valid input: 1 + 2 + 3 + 4 bytes.
        assert_eq!(utf8_length_from_utf16le(&utf16le("aé☕😀")), 10);
        // Unvalidated: an unpaired surrogate counts two bytes.
        assert_eq!(utf8_length_from_utf16le(&[0x00, 0xd8]), 2);
        assert_eq!(utf8_length_from_utf16le(&[0x00, 0xdc]), 2);
        // A trailing odd byte is ignored.
        assert_eq!(utf8_length_from_utf16le(&[0x61, 0x00, 0x62]), 1);
    }

    /// `simdutf::scalar::utf8::utf16_length_from_utf8`, transliterated.
    fn simdutf_utf16_length_from_utf8(source: &[u8]) -> usize {
        source
            .iter()
            .map(|&byte| usize::from(byte.cast_signed() > -65) + usize::from(byte >= 240))
            .sum()
    }

    /// `simdutf::scalar::utf16::utf8_length_from_utf16`, transliterated.
    fn simdutf_utf8_length_from_utf16le(source: &[u8]) -> usize {
        let (units, _) = source.as_chunks::<2>();
        units
            .iter()
            .map(|&unit| {
                let word = u16::from_le_bytes(unit);
                1 + usize::from(word > 0x7f)
                    + usize::from((word > 0x7ff && word <= 0xd7ff) || word >= 0xe000)
            })
            .sum()
    }

    /// A deterministic pseudo-random byte stream (xorshift64).
    fn noise(len: usize) -> Vec<u8> {
        let mut state: u64 = 0x9e37_79b9_7f4a_7c15;
        (0..len)
            .map(|_| {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                state.to_le_bytes()[0]
            })
            .collect()
    }

    #[test]
    fn per_element_counts_match_simdutf_exhaustively() {
        for byte in 0..=u8::MAX {
            assert_eq!(
                usize::from(utf16_units_from_utf8_byte(byte)),
                simdutf_utf16_length_from_utf8(&[byte]),
                "{byte:#04x}"
            );
        }
        for unit in 0..=u16::MAX {
            assert_eq!(
                usize::from(utf8_bytes_from_utf16_unit(unit)),
                simdutf_utf8_length_from_utf16le(&unit.to_le_bytes()),
                "{unit:#06x}"
            );
        }
    }

    /// Exercises the blocked loops: the tail alone, whole blocks plus a tail,
    /// and enough blocks to flush the lane counters more than once.
    #[test]
    fn blocked_length_estimates_match_simdutf() {
        let data = noise(2 * 2 * 21845 * LENGTH_LANES + 3);
        for len in [
            0,
            1,
            LENGTH_LANES - 1,
            LENGTH_LANES,
            LENGTH_LANES + 1,
            127 * LENGTH_LANES,
            127 * LENGTH_LANES + 1,
            2 * 127 * LENGTH_LANES + 5,
            data.len(),
        ] {
            let source = &data[..len];
            assert_eq!(
                utf16_length_from_utf8(source),
                simdutf_utf16_length_from_utf8(source),
                "utf16_length_from_utf8, {len} bytes"
            );
            assert_eq!(
                utf8_length_from_utf16le(source),
                simdutf_utf8_length_from_utf16le(source),
                "utf8_length_from_utf16le, {len} bytes"
            );
        }
    }

    /// Inputs where every element contributes the maximum count, over more
    /// than one flush interval: a lane counter that could overflow would panic
    /// here in a build with overflow checks, and miscount without.
    #[test]
    fn blocked_length_estimates_do_not_overflow_their_lanes() {
        // Four-byte lead bytes count 2 units each.
        let source = vec![0xf0; 2 * 127 * LENGTH_LANES + 1];
        assert_eq!(utf16_length_from_utf8(&source), 2 * source.len());
        // U+0800 counts 3 bytes.
        let source: Vec<u8> = std::iter::repeat_n([0x00, 0x08], 2 * 21845 * LENGTH_LANES + 1)
            .flatten()
            .collect();
        assert_eq!(utf8_length_from_utf16le(&source), 3 * (source.len() / 2));
    }

    #[test]
    fn utf8_to_utf16le_round_trips_valid_input() {
        let text = "aé☕😀";
        let mut target = vec![0; utf16_length_from_utf8(text.as_bytes()) * 2];
        assert_eq!(convert_utf8_to_utf16le(text.as_bytes(), &mut target), 5);
        assert_eq!(target, utf16le(text));
    }

    #[test]
    fn utf8_to_utf16le_rejects_invalid_input() {
        let mut target = [0; 16];
        for invalid in [
            &[0xc3][..],               // truncated
            &[0xc0, 0x80],             // overlong
            &[0xed, 0xa0, 0x80],       // encoded surrogate
            &[0xf4, 0x90, 0x80, 0x80], // above U+10FFFF
            &[0x80],                   // lone continuation byte
        ] {
            assert_eq!(
                convert_utf8_to_utf16le(invalid, &mut target),
                0,
                "{invalid:x?}"
            );
        }
    }

    #[test]
    fn utf16le_to_utf8_round_trips_valid_input() {
        let source = utf16le("aé☕😀");
        let mut target = vec![0; utf8_length_from_utf16le(&source)];
        assert_eq!(convert_utf16le_to_utf8(&source, &mut target), 10);
        assert_eq!(target, "aé☕😀".as_bytes());
    }

    #[test]
    fn utf16le_to_utf8_rejects_unpaired_surrogates() {
        let mut target = [0; 16];
        for invalid in [
            &[0x00, 0xd8][..],         // lone high surrogate
            &[0x00, 0xdc],             // lone low surrogate
            &[0x00, 0xd8, 0x61, 0x00], // high surrogate then non-surrogate
            &[0x61, 0x00, 0x00, 0xdc], // low surrogate after non-surrogate
        ] {
            assert_eq!(
                convert_utf16le_to_utf8(invalid, &mut target),
                0,
                "{invalid:x?}"
            );
        }
    }

    #[test]
    fn conversions_stop_at_the_end_of_a_short_target() {
        let mut target = [0; 2];
        assert_eq!(convert_latin1_to_utf16(b"ab", &mut target), 1);
        assert_eq!(convert_utf8_to_utf16le(b"ab", &mut target), 1);
        assert_eq!(convert_utf16le_to_utf8(&utf16le("a☕"), &mut target), 1);
    }
}
