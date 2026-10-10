// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_test::Harness;

use super::*;

// `Harness::new()` initializes the V8 platform, which is what installs the
// embedded ICU data. ICU converter opens fail without it, even though these
// tests never create an isolate or run JavaScript.
fn init_icu() -> Harness {
    Harness::new()
}

fn utf16le(text: &str) -> Vec<u8> {
    text.encode_utf16().flat_map(u16::to_le_bytes).collect()
}

/// Opens a converter for `to` with the `?` substitute `dispatch` configures.
fn substituting_converter(to: Encoding) -> Converter {
    let converter = Converter::open(to).unwrap();
    converter
        .set_subst_chars(&"?".repeat(converter.min_char_size()))
        .unwrap();
    converter
}

/// Runs [`convert_ex`] the way `dispatch` does, and returns what it wrote.
fn convert(from: Encoding, to: Encoding, source: &[u8]) -> Vec<u8> {
    let to = substituting_converter(to);
    let from = Converter::open(from).unwrap();
    let mut target = vec![0; source.len() * to.max_char_size()];
    let written = convert_ex(&to, &from, source, &mut target).unwrap();
    target.truncate(written);
    target
}

/// Runs [`from_uchars`] the way `dispatch` does, and returns what it wrote.
fn from_utf16le(to: Encoding, source: &[u8]) -> Vec<u8> {
    let to = substituting_converter(to);
    let mut target = vec![0; source.len() / 2 * to.max_char_size()];
    let written = from_uchars(&to, source, &mut target).unwrap();
    target.truncate(written);
    target
}

// `src/workerd/api/node/i18n-test.c++` checks these conversions against the
// C++ path; these pin down the ICU behaviour that path relies on.

#[test]
fn char_sizes_come_from_icu() {
    let _harness = init_icu();
    for (encoding, min, max) in [
        (Encoding::Ascii, 1, 1),
        (Encoding::Latin1, 1, 1),
        (Encoding::Utf8, 1, 3),
        (Encoding::Utf16Le, 2, 2),
    ] {
        let converter = Converter::open(encoding).unwrap();
        assert_eq!(converter.min_char_size(), min, "{encoding:?}");
        assert_eq!(converter.max_char_size(), max, "{encoding:?}");
    }
}

#[test]
fn ascii_and_latin1_are_windows_1252() {
    // Chromium's ICU makes `us-ascii` and `iso8859-1` aliases of
    // windows-1252.
    let _harness = init_icu();
    let euro = "\u{20ac}".as_bytes();
    for encoding in [Encoding::Ascii, Encoding::Latin1] {
        assert_eq!(convert(encoding, Encoding::Utf8, &[0x80]), euro);
        assert_eq!(
            convert(encoding, Encoding::Utf8, &[0x81]),
            "\u{81}".as_bytes()
        );
        assert_eq!(convert(Encoding::Utf8, encoding, euro), [0x80]);
        assert_eq!(
            convert(encoding, encoding, &[0x61, 0x80, 0xff]),
            [0x61, 0x80, 0xff]
        );
    }
}

#[test]
fn ill_formed_input_decodes_to_replacement_characters() {
    const FFFD: &[u8] = "\u{fffd}".as_bytes();
    let _harness = init_icu();
    assert_eq!(
        convert(Encoding::Utf8, Encoding::Utf8, &[0xe2, 0x82, 0x41]),
        [FFFD, b"A"].concat()
    );
    assert_eq!(
        convert(
            Encoding::Utf16Le,
            Encoding::Utf16Le,
            &[0x00, 0xd8, 0x41, 0x00]
        ),
        [0xfd, 0xff, 0x41, 0x00]
    );
}

#[test]
fn unrepresentable_characters_encode_as_question_marks() {
    let _harness = init_icu();
    assert_eq!(
        convert(Encoding::Utf8, Encoding::Latin1, "é☕".as_bytes()),
        [0xe9, b'?']
    );
    // A supplementary character is one character, so one `?`.
    assert_eq!(
        convert(Encoding::Utf8, Encoding::Ascii, "😀".as_bytes()),
        b"?"
    );
    assert_eq!(from_utf16le(Encoding::Ascii, &utf16le("😀")), b"?");
    assert_eq!(
        from_utf16le(Encoding::Latin1, &utf16le("é☕")),
        [0xe9, b'?']
    );
}

#[test]
fn unrepresentable_default_ignorables_are_dropped() {
    let _harness = init_icu();
    assert_eq!(
        convert(
            Encoding::Utf8,
            Encoding::Latin1,
            "a\u{200b}b\u{feff}".as_bytes()
        ),
        b"ab"
    );
    assert_eq!(from_utf16le(Encoding::Ascii, &utf16le("a\u{200b}b")), b"ab");
}

#[test]
fn a_short_target_is_a_failure() {
    let _harness = init_icu();
    let to = substituting_converter(Encoding::Utf8);
    let from = Converter::open(Encoding::Latin1).unwrap();
    let mut target = [0; 2];
    assert_eq!(convert_ex(&to, &from, &[0xe9, 0xe9], &mut target), None);

    let to = substituting_converter(Encoding::Latin1);
    let mut target = [0; 1];
    assert_eq!(from_uchars(&to, &utf16le("ab"), &mut target), None);
}

// simdutf. `src/workerd/api/node/i18n-test.c++` checks these conversions
// against the C++ path; these pin down the behaviour `dispatch` relies on.

#[test]
fn latin1_widens_every_byte() {
    let source: Vec<u8> = (0..=255).collect();
    let mut target = vec![0; source.len() * 2];
    assert_eq!(convert_latin1_to_utf16(&source, &mut target), Some(256));
    let expected: Vec<u8> = (0..=255u16).flat_map(u16::to_le_bytes).collect();
    assert_eq!(target, expected);
}

#[test]
fn utf16_length_from_utf8_counts_lead_bytes() {
    // Exact on valid input: 1 + 1 + 1 + 2 units.
    assert_eq!(Utf8Source::new("aé☕😀".as_bytes()).utf16_len(), 5);
    // Continuation bytes alone count nothing.
    assert_eq!(Utf8Source::new(&[0x80, 0xbf]).utf16_len(), 0);
}

#[test]
fn utf8_length_from_utf16le_is_exact_for_valid_input() {
    // 1 + 2 + 3 + 4 bytes.
    assert_eq!(Utf16LeSource::new(&utf16le("aé☕😀")).utf8_len(), 10);
    // A trailing odd byte is ignored.
    assert_eq!(Utf16LeSource::new(&[0x61, 0x00, 0x62]).utf8_len(), 1);
}

#[test]
fn utf8_to_utf16le_round_trips_valid_input() {
    let text = "aé☕😀";
    let utf8 = Utf8Source::new(text.as_bytes());
    let mut target = vec![0; utf8.utf16_len() * 2];
    assert_eq!(utf8.convert_to_utf16le(&mut target), Some(5));
    assert_eq!(target, utf16le(text));
}

#[test]
fn utf8_to_utf16le_rejects_invalid_input() {
    for invalid in [
        &[0x61, 0xc3][..],         // truncated
        &[0xc0, 0x80],             // overlong
        &[0xed, 0xa0, 0x80],       // encoded surrogate
        &[0xf4, 0x90, 0x80, 0x80], // above U+10FFFF
        &[0x61, 0x80],             // stray continuation byte
    ] {
        let utf8 = Utf8Source::new(invalid);
        let mut target = vec![0; utf8.utf16_len() * 2];
        assert_eq!(
            utf8.convert_to_utf16le(&mut target),
            Some(0),
            "{invalid:x?}"
        );
    }
}

#[test]
fn utf16le_to_utf8_round_trips_valid_input() {
    let source = utf16le("aé☕😀");
    let utf16 = Utf16LeSource::new(&source);
    let mut target = vec![0; utf16.utf8_len()];
    assert_eq!(utf16.convert_to_utf8(&mut target), Some(10));
    assert_eq!(target, "aé☕😀".as_bytes());
}

#[test]
fn utf16le_to_utf8_rejects_unpaired_surrogates() {
    for invalid in [
        &[0x00, 0xd8][..],         // lone high surrogate
        &[0x00, 0xdc],             // lone low surrogate
        &[0x00, 0xd8, 0x61, 0x00], // high surrogate then non-surrogate
        &[0x61, 0x00, 0x00, 0xdc], // low surrogate after non-surrogate
    ] {
        let utf16 = Utf16LeSource::new(invalid);
        let mut target = vec![0; utf16.utf8_len()];
        assert_eq!(utf16.convert_to_utf8(&mut target), Some(0), "{invalid:x?}");
    }
}

#[test]
fn simdutf_conversions_refuse_a_target_shorter_than_the_estimate() {
    let mut target = [0; 2];
    assert_eq!(convert_latin1_to_utf16(b"ab", &mut target), None);
    assert_eq!(Utf8Source::new(b"ab").convert_to_utf16le(&mut target), None);
    assert_eq!(
        Utf16LeSource::new(&utf16le("a☕")).convert_to_utf8(&mut target),
        None
    );
}
