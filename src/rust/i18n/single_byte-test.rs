// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Expected bytes are what Node.js 26's `buffer.transcode()` returns.

use super::*;

const FFFD: &[u8] = "\u{fffd}".as_bytes();

fn utf16le(text: &str) -> Vec<u8> {
    text.encode_utf16().flat_map(u16::to_le_bytes).collect()
}

fn transcode(source: &[u8], from: Encoding, to: Encoding) -> Vec<u8> {
    let mut dest = vec![0; dest_len(source, from, to)];
    let written = transcode_into(source, from, to, &mut dest);
    dest.truncate(written);
    dest
}

#[test]
fn latin1_decodes_every_byte_to_the_same_code_point() {
    assert_eq!(
        transcode(&[0x61, 0x80, 0xff], Encoding::Latin1, Encoding::Latin1),
        [0x61, 0x80, 0xff]
    );
}

#[test]
fn non_ascii_bytes_decode_to_replacement_characters() {
    assert_eq!(
        transcode(&[0x61, 0x80, 0xff], Encoding::Ascii, Encoding::Utf8),
        [b"a", FFFD, FFFD].concat()
    );
    for to in [Encoding::Ascii, Encoding::Latin1] {
        assert_eq!(transcode(&[0x61, 0x80, 0xff], Encoding::Ascii, to), b"a??");
    }
}

#[test]
fn unrepresentable_characters_encode_as_question_marks() {
    assert_eq!(
        transcode("é€".as_bytes(), Encoding::Utf8, Encoding::Latin1),
        [0xe9, b'?']
    );
    assert_eq!(
        transcode(&utf16le("é€"), Encoding::Utf16Le, Encoding::Latin1),
        [0xe9, b'?']
    );
    assert_eq!(
        transcode(&[0x61, 0xe9], Encoding::Latin1, Encoding::Ascii),
        b"a?"
    );
    // A supplementary character is one character, so one `?`.
    assert_eq!(
        transcode("😀".as_bytes(), Encoding::Utf8, Encoding::Ascii),
        b"?"
    );
    assert_eq!(
        transcode(&utf16le("😀"), Encoding::Utf16Le, Encoding::Ascii),
        b"?"
    );
}

#[test]
fn malformed_utf8_is_one_question_mark_per_maximal_subpart() {
    for (source, expected) in [
        (&[0xe2, 0x82][..], &b"?"[..]),
        (&[0xc0, 0xaf], b"??"),
        (&[0xed, 0xa0, 0x80], b"???"),
        (&[0xf4, 0x90, 0x80, 0x80], b"????"),
        (&[0x41, 0xff, 0x42], b"A?B"),
    ] {
        assert_eq!(
            transcode(source, Encoding::Utf8, Encoding::Latin1),
            expected,
            "{source:x?}"
        );
    }
}

#[test]
fn unpaired_surrogates_encode_as_question_marks() {
    for to in [Encoding::Ascii, Encoding::Latin1] {
        assert_eq!(
            transcode(&[0x00, 0xd8, 0x41, 0x00], Encoding::Utf16Le, to),
            b"?A"
        );
        assert_eq!(
            transcode(&[0x00, 0xde, 0x3d, 0xd8], Encoding::Utf16Le, to),
            b"??"
        );
    }
}

#[test]
fn unrepresentable_default_ignorables_are_dropped() {
    assert_eq!(
        transcode(
            "a\u{200b}b\u{feff}".as_bytes(),
            Encoding::Utf8,
            Encoding::Latin1
        ),
        b"ab"
    );
    assert_eq!(
        transcode(&utf16le("\u{feff}A"), Encoding::Utf16Le, Encoding::Ascii),
        b"A"
    );
    // U+00AD SOFT HYPHEN is representable in Latin-1, and dropped only from
    // ASCII.
    assert_eq!(
        transcode("\u{ad}".as_bytes(), Encoding::Utf8, Encoding::Latin1),
        [0xad]
    );
    assert_eq!(
        transcode("\u{ad}".as_bytes(), Encoding::Utf8, Encoding::Ascii),
        b""
    );
}

#[test]
fn sized_input_fills_the_destination() {
    for (source, from, to) in [
        (&[0x41, 0xff, 0xe9][..], Encoding::Ascii, Encoding::Utf8),
        (&[0xff; 3], Encoding::Latin1, Encoding::Ascii),
        (&[0x41, 0x00], Encoding::Utf16Le, Encoding::Latin1),
    ] {
        let len = dest_len(source, from, to);
        assert_eq!(transcode(source, from, to).len(), len, "{from:?} -> {to:?}");
    }
}
