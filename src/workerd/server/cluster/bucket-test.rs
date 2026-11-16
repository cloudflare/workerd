// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(clippy::unwrap_used)]

use super::*;
use crate::hex;
use crate::md5;

#[test]
fn md5_vectors() {
    // RFC 1321's suite: the last two pad into a second block.
    for (text, digest) in [
        ("", "d41d8cd98f00b204e9800998ecf8427e"),
        ("a", "0cc175b9c0f1b6a831c399e269772661"),
        ("abc", "900150983cd24fb0d6963f7d28e17f72"),
        ("message digest", "f96b697d7cb7938d525a2f31aaf161d0"),
        (
            "abcdefghijklmnopqrstuvwxyz",
            "c3fcd3d76192e4007dfb496cca67e13b",
        ),
        (
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
            "d174ab98d277d9f5a5611c2c9f419d9f",
        ),
        (
            "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
            "57edf4a22be3c955ac49da2e2107b67a",
        ),
    ] {
        assert_eq!(hex(&md5(text.as_bytes())), digest, "{text}");
    }
}

#[test]
fn ranges() {
    let from = |offset, length| Range::From { offset, length };
    assert_eq!(from(2, Some(3)).header(), "bytes=2-4");
    assert_eq!(from(2, None).header(), "bytes=2-");
    assert_eq!(Range::Suffix(3).header(), "bytes=-3");
    for (range, of) in [
        (from(2, Some(3)), Some((2, 3))),
        (from(8, Some(100)), Some((8, 2))),
        (from(8, None), Some((8, 2))),
        (from(10, None), None),
        (Range::Suffix(3), Some((7, 3))),
        (Range::Suffix(100), Some((0, 10))),
        (Range::Suffix(0), None),
    ] {
        assert_eq!(range.of(10), of, "{range:?}");
    }
    assert_eq!(Range::Suffix(1).of(0), None);
}
