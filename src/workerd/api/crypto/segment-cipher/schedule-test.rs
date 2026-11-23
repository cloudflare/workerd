// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn template(index: IndexEncoding) -> InfoTemplate {
    InfoTemplate {
        prefix: b"seg:".to_vec(),
        index,
        suffix: b":end".to_vec(),
    }
}

#[test]
fn info_encodings() {
    assert_eq!(template(IndexEncoding::Decimal).build(0), b"seg:0:end");
    assert_eq!(
        template(IndexEncoding::Decimal).build(u64::MAX),
        b"seg:18446744073709551615:end"
    );
    assert_eq!(
        template(IndexEncoding::Uint32Be).build(0x0102_0304),
        b"seg:\x01\x02\x03\x04:end"
    );
    assert_eq!(
        template(IndexEncoding::Uint64Be).build(1),
        b"seg:\0\0\0\0\0\0\0\x01:end"
    );
    assert!(IndexEncoding::parse("hex").is_err());
}

// RFC 5869 test case 1, HKDF-Expand with SHA-256. Its info is f0..f9: a prefix f0..f5 and the
// index 0xf6f7f8f9 as uint32be.
#[test]
fn hkdf_expand_vector() {
    let schedule = KeySchedule {
        digest: ffi::Digest::Sha256,
        prk: Secret::new(hex(
            "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
        )),
    };
    let info = InfoTemplate {
        prefix: hex("f0f1f2f3f4f5"),
        index: IndexEncoding::Uint32Be,
        suffix: Vec::new(),
    };
    let key = schedule.derive(&info, 0xf6f7_f8f9, 42).unwrap();
    assert_eq!(
        key.as_slice(),
        hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865")
    );
}

fn hex(digits: &str) -> Vec<u8> {
    (0..digits.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&digits[i..i + 2], 16).unwrap())
        .collect()
}
