// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

/// A deterministic pseudo-random byte generator (xorshift), so failures reproduce.
struct Bytes(u64);

impl Bytes {
    fn next(&mut self) -> u8 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        (self.0 >> 24) as u8
    }

    fn pick<'a>(&mut self, choices: &'a [u8]) -> u8 {
        choices[usize::from(self.next()) % choices.len()]
    }

    fn string(&mut self, choices: &[u8], len: usize) -> Vec<u8> {
        (0..len).map(|_| self.pick(choices)).collect()
    }
}

/// simdutf's `base64_default_or_url_accept_garbage` decoding, written for clarity.
fn reference_forgiving(src: &[u8]) -> Vec<u8> {
    let src = src.split(|&c| c == b'=').next().unwrap();
    let values: Vec<u32> = src
        .iter()
        .map(|&c| BASE64_VALUES[usize::from(c)])
        .filter(|&value| value != INVALID)
        .map(u32::from)
        .collect();
    let mut out = Vec::new();
    for group in values.chunks(4) {
        let bits = group
            .iter()
            .enumerate()
            .fold(0u32, |bits, (i, &value)| bits | value << (18 - 6 * i));
        let bytes = [(bits >> 16) as u8, (bits >> 8) as u8, bits as u8];
        out.extend_from_slice(&bytes[..group.len().saturating_sub(1)]);
    }
    out
}

fn forgiving(src: &[u8]) -> Vec<u8> {
    let mut dst = vec![0; maximal_decoded_length(src)];
    let written = decode_base64_forgiving(src, &mut dst);
    dst.truncate(written);
    dst
}

fn legacy(src: &[u8], dst_len: usize) -> Vec<u8> {
    let mut dst = vec![0; dst_len];
    let written = decode_base64_legacy(&mut dst, src);
    dst.truncate(written);
    dst
}

#[test]
fn maximal_decoded_length_matches_simdutf() {
    assert_eq!(maximal_decoded_length(b""), 0);
    assert_eq!(maximal_decoded_length(b"Q"), 0);
    assert_eq!(maximal_decoded_length(b"QQ"), 1);
    assert_eq!(maximal_decoded_length(b"QQ=="), 1);
    assert_eq!(maximal_decoded_length(b"QUI"), 2);
    assert_eq!(maximal_decoded_length(b"QUI="), 2);
    assert_eq!(maximal_decoded_length(b"QUJD"), 3);
    assert_eq!(maximal_decoded_length(b"QUJDQ"), 3);
    assert_eq!(maximal_decoded_length(b"=="), 0);
}

#[test]
fn forgiving_decodes_encoder_output() {
    assert_eq!(forgiving(b""), b"");
    assert_eq!(forgiving(b"QUJD"), b"ABC");
    assert_eq!(forgiving(b"QUI="), b"AB");
    assert_eq!(forgiving(b"QQ=="), b"A");
    assert_eq!(forgiving(b"QUI"), b"AB");
    assert_eq!(forgiving(b"QQ"), b"A");
    // URL-safe alphabet: 0xfb 0xff 0xbf.
    assert_eq!(forgiving(b"-_-_"), [0xfb, 0xff, 0xbf]);
    assert_eq!(forgiving(b"+/+/"), [0xfb, 0xff, 0xbf]);
}

#[test]
fn forgiving_skips_garbage_and_stops_at_padding() {
    assert_eq!(forgiving(b"QU JD\n"), b"ABC");
    assert_eq!(forgiving(b"Q*U*J*D"), b"ABC");
    assert_eq!(forgiving(b"QQ==QUJD"), b"A");
    assert_eq!(forgiving(b"=QUJD"), b"");
    // Mixed alphabets.
    assert_eq!(forgiving(b"-/+_"), [0xfb, 0xff, 0xbf]);
    // A final group of one character decodes to nothing.
    assert_eq!(forgiving(b"QUJDQ"), b"ABC");
    assert_eq!(forgiving(b"Q"), b"");
    // Bits left over by a final group are ignored.
    assert_eq!(forgiving(b"QR"), b"A");
    assert_eq!(forgiving(b"QUJ"), b"AB");
    // Non-ASCII bytes are garbage.
    assert_eq!(forgiving(b"Q\xc1U\xffJD"), b"ABC");
}

#[test]
fn forgiving_matches_reference() {
    let mut bytes = Bytes(0x9e37_79b9_7f4a_7c15);
    let clean = STANDARD_ALPHABET;
    let messy: Vec<u8> = STANDARD_ALPHABET
        .iter()
        .copied()
        .chain(*b"-_-_ \n\r\t=*.\x80\xff")
        .collect();
    for len in 0..300 {
        for choices in [&clean[..], &messy[..]] {
            let src = bytes.string(choices, len);
            assert_eq!(forgiving(&src), reference_forgiving(&src), "input {src:?}");
        }
    }
}

#[test]
fn legacy_decodes_like_nbytes() {
    assert_eq!(legacy(b"QUJD", 10), b"ABC");
    assert_eq!(legacy(b"QUI=", 10), b"AB");
    assert_eq!(legacy(b"QQ==", 10), b"A");
    assert_eq!(legacy(b"QQ", 10), b"A");
    assert_eq!(legacy(b"Q", 10), b"");
    assert_eq!(legacy(b"", 10), b"");
    assert_eq!(legacy(b"QU JD", 10), b"ABC");
    assert_eq!(legacy(b"QUJD QUJD", 10), b"ABCABC");
    assert_eq!(legacy(b"-_-_", 10), [0xfb, 0xff, 0xbf]);
    // An `=` stops the slow path.
    assert_eq!(legacy(b"QQ=*QUJD", 10), b"A");
    // Decoding stops when the destination is full.
    assert_eq!(legacy(b"QUJDQUJD", 4), b"ABCA");
    assert_eq!(legacy(b"QUJDQUJD", 0), b"");
}

#[test]
fn legacy_agrees_with_forgiving_on_clean_input() {
    let mut bytes = Bytes(0x2545_f491_4f6c_dd1d);
    for len in 0..200 {
        let src = bytes.string(STANDARD_ALPHABET, len);
        if len % 4 == 1 {
            continue;
        }
        assert_eq!(legacy(&src, len), forgiving(&src), "input {src:?}");
    }
}

/// `SearchString`'s contract, written for clarity.
fn reference_search(haystack: &[u16], needle: &[u16], start: usize, is_forward: bool) -> usize {
    let matches = |at: usize| haystack[at..].starts_with(needle);
    let last = haystack.len() - needle.len();
    let found = if is_forward {
        (start..=last).find(|&at| matches(at))
    } else {
        (0..=start.min(last)).rev().find(|&at| matches(at))
    };
    found.unwrap_or(haystack.len())
}

#[test]
fn search_one_byte_matches_reference() {
    let mut bytes = Bytes(0xdead_beef_cafe_f00d);
    for _ in 0..2000 {
        let haystack_len = 1 + usize::from(bytes.next()) % 40;
        let haystack = bytes.string(b"ab", haystack_len);
        let needle_len = 1 + usize::from(bytes.next()) % haystack.len().min(4);
        let needle = bytes.string(b"ab", needle_len);
        let wide = |s: &[u8]| s.iter().map(|&c| u16::from(c)).collect::<Vec<_>>();
        for is_forward in [true, false] {
            let limit = if is_forward {
                haystack.len() - needle.len()
            } else {
                haystack.len() + 2
            };
            for start in 0..=limit {
                assert_eq!(
                    search_one_byte(&haystack, &needle, start, is_forward),
                    reference_search(&wide(&haystack), &wide(&needle), start, is_forward),
                    "{haystack:?} {needle:?} {start} {is_forward}"
                );
            }
        }
    }
}

#[test]
fn search_two_byte_skips_odd_offsets() {
    // A needle of the haystack's middle two bytes matches them only at an odd offset, which is
    // not a match of code units.
    let haystack: Vec<u8> = [0x6162u16, 0x6100]
        .iter()
        .flat_map(|u| u.to_ne_bytes())
        .collect();
    let needle = [haystack[1], haystack[2]];
    assert_eq!(search_two_byte(&haystack, &needle, 0, true), 2);
    assert_eq!(search_two_byte(&haystack, &needle, 1, false), 2);
    // The same bytes at an even offset do match.
    let haystack = [0, 0, needle[0], needle[1]];
    assert_eq!(search_two_byte(&haystack, &needle, 0, true), 1);
    assert_eq!(search_two_byte(&haystack, &needle, 1, false), 1);
}

#[test]
fn search_two_byte_matches_reference() {
    let mut bytes = Bytes(0x0123_4567_89ab_cdef);
    // Units whose bytes repeat across unit boundaries, to produce odd-offset byte matches.
    let units = [0x0101u16, 0x0102, 0x0201, 0x0202];
    for _ in 0..2000 {
        let haystack_len = 1 + usize::from(bytes.next()) % 30;
        let haystack: Vec<u16> = (0..haystack_len)
            .map(|_| units[usize::from(bytes.next()) % units.len()])
            .collect();
        let needle_len = 1 + usize::from(bytes.next()) % haystack.len().min(4);
        let needle: Vec<u16> = (0..needle_len)
            .map(|_| units[usize::from(bytes.next()) % units.len()])
            .collect();
        let as_bytes = |s: &[u16]| s.iter().flat_map(|u| u.to_ne_bytes()).collect::<Vec<_>>();
        for is_forward in [true, false] {
            let limit = if is_forward {
                haystack.len() - needle.len()
            } else {
                haystack.len() + 2
            };
            for start in 0..=limit {
                assert_eq!(
                    search_two_byte(&as_bytes(&haystack), &as_bytes(&needle), start, is_forward),
                    reference_search(&haystack, &needle, start, is_forward),
                    "{haystack:?} {needle:?} {start} {is_forward}"
                );
            }
        }
    }
}

#[test]
fn hex_pairs_are_lowercase_digits() {
    assert_eq!(HEX_PAIRS[0x00], *b"00");
    assert_eq!(HEX_PAIRS[0x0f], *b"0f");
    assert_eq!(HEX_PAIRS[0xa5], *b"a5");
    assert_eq!(HEX_PAIRS[0xff], *b"ff");
}

#[test]
fn range_checks_bounds() {
    assert_eq!(range(10, 2.0, 5.0).unwrap(), 2..5);
    assert_eq!(range(10, 0.0, 10.0).unwrap(), 0..10);
    assert_eq!(range(10, 2.5, 5.9).unwrap(), 2..5);
    assert!(range(10, 5.0, 2.0).is_err());
    assert!(range(10, -1.0, 2.0).is_err());
    assert!(range(10, 0.0, 11.0).is_err());
    assert!(range(10, f64::NAN, 2.0).is_err());
}

#[test]
fn search_start_checks_arguments() {
    assert_eq!(search_start(10, 2, 8.0, true).unwrap(), 8);
    assert!(search_start(10, 2, 9.0, true).is_err());
    assert_eq!(search_start(10, 2, 20.0, false).unwrap(), 20);
    assert!(search_start(10, 0, 0.0, true).is_err());
    assert!(search_start(2, 3, 0.0, false).is_err());
    assert!(search_start(10, 2, -1.0, false).is_err());
    assert!(search_start(10, 2, f64::NAN, false).is_err());
}
