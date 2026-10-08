// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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
