// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn utf16le(text: &str) -> Vec<u8> {
    text.encode_utf16().flat_map(u16::to_le_bytes).collect()
}

// simdutf. These pin down the simdutf behaviour `dispatch` relies on.

#[test]
fn ascii_prefix_stops_at_the_first_non_ascii_byte() {
    let long = [&[b'a'; 100][..], &[0xff, b'a']].concat();
    let cases: [&[u8]; 5] = [
        b"",
        b"plain ascii",
        "caf\u{e9}".as_bytes(),
        &[0x80, 0x41],
        &long,
    ];
    for source in cases {
        let ascii = source.iter().take_while(|byte| byte.is_ascii()).count();
        assert_eq!(ascii_prefix_len(source), ascii, "{source:x?}");
    }
}

#[test]
fn utf8_valid_prefix_matches_the_standard_library() {
    let long = [&[b'a'; 100][..], "\u{e9}".as_bytes(), &[0xe2, 0x82, 0x41]].concat();
    let cases: [&[u8]; 7] = [
        b"",
        b"plain ascii",
        "caf\u{e9}\u{1f600}".as_bytes(),
        &[0x41, 0x80],
        &[0xed, 0xa0, 0x80],
        &[0xc3, 0xa9, 0xe2, 0x82],
        &long,
    ];
    for source in cases {
        let valid = str::from_utf8(source).map_or_else(|error| error.valid_up_to(), str::len);
        assert_eq!(utf8_valid_prefix_len(source), valid, "{source:x?}");
    }
}

#[test]
fn utf16le_valid_prefix_stops_at_unpaired_surrogates_and_odd_bytes() {
    let valid = utf16le("a\u{1f600}\u{feff}");
    assert_eq!(utf16le_valid_prefix_len(&valid), valid.len());
    assert_eq!(
        utf16le_valid_prefix_len(&[0x41, 0x00, 0x00, 0xd8, 0x41, 0x00]),
        2
    );
    assert_eq!(utf16le_valid_prefix_len(&[0x00, 0xdc]), 0);
    assert_eq!(utf16le_valid_prefix_len(&[0x41, 0x00, 0x42]), 2);
    assert_eq!(utf16le_valid_prefix_len(&[0x41, 0x00, 0x00, 0xd8, 0x42]), 2);
}

#[test]
fn latin1_to_utf8_encodes_every_byte() {
    let source: Vec<u8> = (0..=255).collect();
    let expected: String = source.iter().map(|&byte| char::from(byte)).collect();
    let latin1 = Latin1Source::new(&source);
    assert_eq!(latin1.utf8_len(), expected.len());
    let mut target = vec![0; latin1.utf8_len()];
    assert_eq!(latin1.convert_to_utf8(&mut target), Some(expected.len()));
    assert_eq!(target, expected.as_bytes());
    assert_eq!(latin1.convert_to_utf8(&mut target[1..]), None);
}

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
