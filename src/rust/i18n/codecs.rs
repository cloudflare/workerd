// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The codec primitives behind [`crate::dispatch`], all in safe Rust.
//!
//! [`transcode_substituting`] reproduces the conversions `i18n.c++` hands to
//! ICU, and the functions after it reproduce the simdutf calls it makes. The
//! transcoding *logic* -- dispatch, sizing, validation, truncation -- lives in
//! [`crate::dispatch`].

use std::char::REPLACEMENT_CHARACTER;

use crate::ffi::Encoding;

// The bridge `Encoding` is a `cxx` shared enum, and so a `u8` newtype rather
// than a real Rust enum: every `match` on it needs a wildcard arm. As in
// `i18n.c++`, whose `getEncodingName` ends in `default: KJ_UNREACHABLE`,
// `dispatch` rejects an unknown source encoding up front and every later match
// treats one as unreachable. The C++ `fromImpl` conversion rejects anything
// but the four transcodable encodings before Rust is entered, and a panic in
// Rust reaches C++ as a `kj::Exception`.

/// The most bytes one UTF-16 code unit can occupy in `encoding`, which is what
/// `ucnv_getMaxCharSize` reports for the ICU converter `i18n.c++` opens for it
/// (`Converter::maxCharSize`). `i18n.c++` sizes its ICU conversions'
/// destinations from this, and the size is visible to JavaScript as the
/// result's `buffer.byteLength`, so `dispatch` must size them the same way.
///
/// ICU measures per UTF-16 code unit, which is why UTF-8's maximum is 3 rather
/// than 4: a supplementary character is two code units and four bytes.
pub fn max_char_size(encoding: Encoding) -> usize {
    match encoding {
        Encoding::Ascii | Encoding::Latin1 => 1,
        Encoding::Utf8 => 3,
        Encoding::Utf16Le => 2,
        _ => unreachable!("invalid encoding {encoding:?}"),
    }
}

/// The characters windows-1252 assigns to bytes `0x80`-`0x9f`, per the WHATWG
/// Encoding Standard's `index-windows-1252`. Every other byte maps to the code
/// point of the same value, as in Latin-1; so do the five bytes the index
/// leaves unassigned in this range (`0x81`, `0x8d`, `0x8f`, `0x90`, `0x9d`),
/// whose entries here are those C1 controls.
const WINDOWS_1252_HIGH: [char; 32] = [
    '\u{20ac}', '\u{0081}', '\u{201a}', '\u{0192}', '\u{201e}', '\u{2026}', '\u{2020}', '\u{2021}',
    '\u{02c6}', '\u{2030}', '\u{0160}', '\u{2039}', '\u{0152}', '\u{008d}', '\u{017d}', '\u{008f}',
    '\u{0090}', '\u{2018}', '\u{2019}', '\u{201c}', '\u{201d}', '\u{2022}', '\u{2013}', '\u{2014}',
    '\u{02dc}', '\u{2122}', '\u{0161}', '\u{203a}', '\u{0153}', '\u{009d}', '\u{017e}', '\u{0178}',
];

/// Decodes a windows-1252 byte. Every byte is assigned.
fn windows_1252_decode(byte: u8) -> char {
    match byte {
        0x80..=0x9f => WINDOWS_1252_HIGH[usize::from(byte - 0x80)],
        _ => char::from(byte),
    }
}

/// Encodes `ch` as windows-1252, if it has a mapping.
fn windows_1252_encode(ch: char) -> Option<u8> {
    match u32::from(ch) {
        0x00..=0x7f | 0xa0..=0xff => u8::try_from(ch).ok(),
        _ => WINDOWS_1252_HIGH
            .iter()
            .position(|&high| high == ch)
            .and_then(|index| u8::try_from(0x80 + index).ok()),
    }
}

/// Whether ICU's substitute callback drops `ch`, rather than substituting it,
/// when a converter cannot map it: ICU's hard-coded list of default-ignorable
/// code points, `IS_DEFAULT_IGNORABLE_CODE_POINT` in ICU's `ucnv_err.cpp`.
const fn is_icu_default_ignorable(ch: char) -> bool {
    matches!(
        ch as u32,
        0x00ad
            | 0x034f
            | 0x061c
            | 0x115f
            | 0x1160
            | 0x17b4..=0x17b5
            | 0x180b..=0x180f
            | 0x200b..=0x200f
            | 0x202a..=0x202e
            | 0x2060..=0x206f
            | 0x3164
            | 0xfe00..=0xfe0f
            | 0xfeff
            | 0xffa0
            | 0xfff0..=0xfff8
            | 0x1bca0..=0x1bca3
            | 0x1d173..=0x1d17a
            | 0xe0000..=0xe0fff
    )
}

/// Converts `source` from `from` to `to` the way `i18n.c++`'s ICU conversions
/// do (`ucnv_convertEx` in `TranscodeDefault`, `ucnv_fromUChars` in
/// `TranscodeFromUTF16`), writing into `target` and returning the number of
/// bytes written. Never fails: malformed or unmappable input is substituted.
///
/// `i18n.c++` opens ICU converters named `us-ascii` and `iso8859-1` for ASCII
/// and Latin-1, but workerd links Chromium's ICU, whose converter aliases
/// follow the WHATWG Encoding Standard and make both names aliases of
/// `windows-1252`. So on this path, and only this one, both encodings are
/// windows-1252. (The Latin-1 to UTF-16 conversion below is true Latin-1, as
/// it is in `i18n.c++`.)
///
/// ICU converts through Unicode, substituting in each direction with the
/// callbacks `i18n.c++` leaves in place:
///
/// - Decoding `from`, each ill-formed sequence becomes U+FFFD: each maximal
///   subpart of ill-formed UTF-8, as [`<[u8]>::utf8_chunks`] splits them; an
///   unpaired UTF-16 surrogate; and a trailing odd byte of UTF-16LE, together
///   with the lead surrogate before it if there is one. Every windows-1252
///   byte is well-formed.
/// - Encoding into `to`, each character the encoding cannot represent becomes
///   `?`, the substitute `i18n.c++` configures, unless it is one of ICU's
///   default-ignorable code points, which is dropped. Only windows-1252 has
///   unrepresentable characters; UTF-8 and UTF-16LE represent everything,
///   U+FFFD included.
///
/// Stops at the end of `target` rather than overflowing it, though `dispatch`
/// sizes it to always fit.
pub fn transcode_substituting(
    from: Encoding,
    to: Encoding,
    source: &[u8],
    target: &mut [u8],
) -> usize {
    match from {
        Encoding::Ascii | Encoding::Latin1 => {
            encode_substituting(to, source.iter().copied().map(windows_1252_decode), target)
        }
        Encoding::Utf8 => {
            let chars = source.utf8_chunks().flat_map(|chunk| {
                let replacement = (!chunk.invalid().is_empty()).then_some(REPLACEMENT_CHARACTER);
                chunk.valid().chars().chain(replacement)
            });
            encode_substituting(to, chars, target)
        }
        Encoding::Utf16Le => {
            let (mut units, odd_byte) = source.as_chunks::<2>();
            // ICU reads a lead surrogate followed by a lone trailing byte as
            // one truncated sequence, for one U+FFFD rather than two.
            if !odd_byte.is_empty()
                && let Some((&last, rest)) = units.split_last()
                && (0xd800..=0xdbff).contains(&u16::from_le_bytes(last))
            {
                units = rest;
            }
            let chars = char::decode_utf16(units.iter().map(|&unit| u16::from_le_bytes(unit)))
                .map(|ch| ch.unwrap_or(REPLACEMENT_CHARACTER))
                .chain((!odd_byte.is_empty()).then_some(REPLACEMENT_CHARACTER));
            encode_substituting(to, chars, target)
        }
        _ => unreachable!("invalid encoding {from:?}"),
    }
}

/// Encodes `chars` into `to`, substituting `?` for, or dropping, any character
/// `to` cannot represent. Returns the number of bytes written, stopping at the
/// end of `target`.
fn encode_substituting(
    to: Encoding,
    chars: impl Iterator<Item = char>,
    target: &mut [u8],
) -> usize {
    let mut written = 0;
    let mut buf = [0u8; 4];
    for ch in chars {
        let bytes: &[u8] = match to {
            Encoding::Ascii | Encoding::Latin1 => match windows_1252_encode(ch) {
                Some(byte) => {
                    buf[0] = byte;
                    &buf[..1]
                }
                None if is_icu_default_ignorable(ch) => &[],
                None => b"?",
            },
            Encoding::Utf8 => ch.encode_utf8(&mut buf).as_bytes(),
            Encoding::Utf16Le => {
                let mut units = [0u16; 2];
                let units = ch.encode_utf16(&mut units);
                for (out, unit) in buf.as_chunks_mut::<2>().0.iter_mut().zip(units.iter()) {
                    *out = unit.to_le_bytes();
                }
                &buf[..units.len() * 2]
            }
            _ => unreachable!("invalid encoding {to:?}"),
        };
        let Some(out) = target.get_mut(written..written + bytes.len()) else {
            break;
        };
        out.copy_from_slice(bytes);
        written += bytes.len();
    }
    written
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
    use super::*;

    fn utf16le(text: &str) -> Vec<u8> {
        text.encode_utf16().flat_map(u16::to_le_bytes).collect()
    }

    /// Runs [`transcode_substituting`] into a destination sized the way
    /// `dispatch` sizes it for `ucnv_convertEx`, and returns what it wrote.
    fn substituting(from: Encoding, to: Encoding, source: &[u8]) -> Vec<u8> {
        let mut target = vec![0; source.len() * max_char_size(to)];
        let written = transcode_substituting(from, to, source, &mut target);
        target.truncate(written);
        target
    }

    // `src/workerd/api/node/i18n-test.c++` checks `transcode_substituting`
    // against the C++ path; these pin down the rules its doc comment
    // describes.

    #[test]
    fn ascii_and_latin1_are_windows_1252() {
        let euro = "\u{20ac}".as_bytes();
        for encoding in [Encoding::Ascii, Encoding::Latin1] {
            // Decoding: 0x80 is the euro sign; unassigned 0x81 is U+0081.
            assert_eq!(substituting(encoding, Encoding::Utf8, &[0x80]), euro);
            assert_eq!(
                substituting(encoding, Encoding::Utf8, &[0x81]),
                "\u{81}".as_bytes()
            );
            assert_eq!(
                substituting(encoding, Encoding::Utf8, &[0xe9]),
                "é".as_bytes()
            );
            // Encoding is the inverse, and high bytes pass through identity pairs.
            assert_eq!(substituting(Encoding::Utf8, encoding, euro), [0x80]);
            assert_eq!(
                substituting(encoding, encoding, &[0x61, 0x80, 0xff]),
                [0x61, 0x80, 0xff]
            );
        }
        assert_eq!(
            substituting(Encoding::Ascii, Encoding::Latin1, &[0x8d, 0xe9]),
            [0x8d, 0xe9]
        );
    }

    #[test]
    fn ill_formed_input_decodes_to_replacement_characters() {
        const FFFD: &[u8] = "\u{fffd}".as_bytes();
        // One U+FFFD per maximal subpart.
        assert_eq!(
            substituting(Encoding::Utf8, Encoding::Utf8, &[0xf4, 0x90, 0x80, 0x80]),
            FFFD.repeat(4)
        );
        assert_eq!(
            substituting(Encoding::Utf8, Encoding::Utf8, &[0xe2, 0x82, 0x41]),
            [FFFD, b"A"].concat()
        );
        assert_eq!(
            substituting(Encoding::Utf8, Encoding::Utf8, &[0xed, 0xa0, 0x80]),
            FFFD.repeat(3)
        );
        // Unpaired surrogates, and a trailing odd byte.
        assert_eq!(
            substituting(
                Encoding::Utf16Le,
                Encoding::Utf16Le,
                &[0x00, 0xd8, 0x41, 0x00]
            ),
            [0xfd, 0xff, 0x41, 0x00]
        );
        assert_eq!(
            substituting(Encoding::Utf16Le, Encoding::Utf16Le, &[0x41, 0x00, 0x42]),
            [0x41, 0x00, 0xfd, 0xff]
        );
        // A lead surrogate and a trailing odd byte are one truncated sequence;
        // a trail surrogate and one are two.
        assert_eq!(
            substituting(Encoding::Utf16Le, Encoding::Utf16Le, &[0x00, 0xd8, 0x42]),
            [0xfd, 0xff]
        );
        assert_eq!(
            substituting(Encoding::Utf16Le, Encoding::Utf16Le, &[0x00, 0xdc, 0x42]),
            [0xfd, 0xff, 0xfd, 0xff]
        );
    }

    #[test]
    fn unrepresentable_characters_encode_as_question_marks() {
        // Including U+FFFD from ill-formed input, and C1 controls windows-1252
        // assigns other characters to.
        assert_eq!(
            substituting(Encoding::Utf8, Encoding::Latin1, &[0xff]),
            b"?"
        );
        assert_eq!(
            substituting(Encoding::Utf8, Encoding::Latin1, "é☕".as_bytes()),
            [0xe9, b'?']
        );
        assert_eq!(
            substituting(Encoding::Utf8, Encoding::Ascii, "\u{80}".as_bytes()),
            b"?"
        );
        // A supplementary character is one character, so one `?`.
        assert_eq!(
            substituting(Encoding::Utf8, Encoding::Ascii, "😀".as_bytes()),
            b"?"
        );
        assert_eq!(
            substituting(Encoding::Utf16Le, Encoding::Ascii, &utf16le("😀")),
            b"?"
        );
        assert_eq!(
            substituting(
                Encoding::Utf16Le,
                Encoding::Ascii,
                &[0x00, 0xd8, 0x41, 0x00]
            ),
            b"?A"
        );
    }

    #[test]
    fn unrepresentable_default_ignorables_are_dropped() {
        assert_eq!(
            substituting(
                Encoding::Utf8,
                Encoding::Latin1,
                "a\u{200b}b\u{feff}".as_bytes()
            ),
            b"ab"
        );
        assert_eq!(
            substituting(Encoding::Utf8, Encoding::Ascii, "\u{e0001}".as_bytes()),
            b""
        );
        // U+00AD is on ICU's list, but windows-1252 represents it.
        assert_eq!(
            substituting(Encoding::Utf8, Encoding::Latin1, "\u{ad}".as_bytes()),
            [0xad]
        );
    }

    #[test]
    fn transcode_substituting_stops_at_the_end_of_a_short_target() {
        let mut target = [0; 2];
        assert_eq!(
            transcode_substituting(Encoding::Latin1, Encoding::Utf8, &[0xe9, 0xe9], &mut target),
            2
        );
        assert_eq!(target, [0xc3, 0xa9]);
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
