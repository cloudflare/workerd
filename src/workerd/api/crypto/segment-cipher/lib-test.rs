// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn info() -> ffi::KeyInfo {
    ffi::KeyInfo {
        prefix: b"segment ".to_vec(),
        index: "decimal".to_owned(),
        suffix: Vec::new(),
    }
}

fn options(algorithm: &str) -> ffi::CipherOptions {
    ffi::CipherOptions {
        algorithm: algorithm.to_owned(),
        segment_size: 65536.0,
        derive: "HKDF".to_owned(),
        hash: "SHA-256".to_owned(),
        info: info(),
        has_mac_info: algorithm == "AES-CBC-HMAC-SHA256",
        mac_info: info(),
        has_key_bits: false,
        key_bits: 0,
        has_first_segment: false,
        first_segment: 0.0,
        has_plaintext_length: false,
        plaintext_length: 0.0,
        has_output_offset: false,
        output_offset: 0.0,
        has_output_length: false,
        output_length: 0.0,
    }
}

const PRK: [u8; 32] = [9; 32];

fn decrypt_options(plaintext_length: f64) -> ffi::CipherOptions {
    ffi::CipherOptions {
        has_plaintext_length: true,
        plaintext_length,
        ..options("AES-GCM")
    }
}

fn error_of<T>(result: Result<T>) -> String {
    match result {
        Ok(_) => panic!("expected an error"),
        Err(error) => error.description().to_owned(),
    }
}

#[test]
fn creates_transforms() {
    for algorithm in ["AES-GCM", "ChaCha20-Poly1305", "AES-CBC-HMAC-SHA256"] {
        let encryptor = new_encryptor(options(algorithm), &PRK).unwrap();
        assert_eq!(encryptor.high_water_mark(), 65536);
        assert!(encryptor.output_length().is_none());
        // Only the key schedule is held before anything is pushed.
        let retained = encryptor.retained_bytes();
        assert!(retained > 0 && retained < 1024, "{retained}");
    }
    let decryptor = new_decryptor(decrypt_options(100_000.0), &PRK).unwrap();
    assert_eq!(decryptor.high_water_mark(), 65536 + 28);
    assert_eq!(
        Option::<u64>::from(decryptor.output_length()),
        Some(100_000)
    );
}

#[test]
fn output_range() {
    let decryptor = new_decryptor(
        ffi::CipherOptions {
            has_output_offset: true,
            output_offset: 10.0,
            has_output_length: true,
            output_length: 1e9,
            ..decrypt_options(100.0)
        },
        &PRK,
    )
    .unwrap();
    assert_eq!(Option::<u64>::from(decryptor.output_length()), Some(90));
}

#[test]
fn rejects_invalid_options() {
    let cases: [(ffi::CipherOptions, bool, &str); 10] = [
        (
            options("AES-GCM"),
            false,
            "jsg.TypeError: SegmentDecryptionStream requires",
        ),
        (
            ffi::CipherOptions {
                has_output_offset: true,
                ..options("AES-GCM")
            },
            true,
            "jsg.TypeError: outputOffset and outputLength",
        ),
        (
            ffi::CipherOptions {
                has_output_offset: true,
                output_offset: 11.0,
                ..decrypt_options(10.0)
            },
            false,
            "jsg.RangeError: outputOffset (11) is past",
        ),
        (
            ffi::CipherOptions {
                segment_size: 1.5,
                ..options("AES-GCM")
            },
            true,
            "jsg.RangeError: segmentSize must be",
        ),
        (
            ffi::CipherOptions {
                segment_size: 33_554_432.0,
                ..options("AES-GCM")
            },
            true,
            "jsg.RangeError: segmentSize must be between",
        ),
        (
            ffi::CipherOptions {
                has_key_bits: true,
                key_bits: 192,
                ..options("AES-GCM")
            },
            true,
            "jsg.DOMException(OperationError): AES-GCM segment keys",
        ),
        (
            ffi::CipherOptions {
                derive: "PBKDF2".to_owned(),
                ..options("AES-GCM")
            },
            true,
            "jsg.DOMException(NotSupportedError): Unrecognized segment key derivation",
        ),
        (
            ffi::CipherOptions {
                info: ffi::KeyInfo {
                    index: "hex".to_owned(),
                    ..info()
                },
                ..options("AES-GCM")
            },
            true,
            "jsg.TypeError: Unrecognized segment index encoding",
        ),
        (
            ffi::CipherOptions {
                has_mac_info: false,
                ..options("AES-CBC-HMAC-SHA256")
            },
            true,
            "jsg.TypeError: AES-CBC-HMAC-SHA256 requires macInfo.",
        ),
        (
            ffi::CipherOptions {
                has_first_segment: true,
                first_segment: 4_294_967_296.0,
                info: ffi::KeyInfo {
                    index: "uint32be".to_owned(),
                    ..info()
                },
                ..options("AES-GCM")
            },
            true,
            "jsg.RangeError: The segment indexes exceed",
        ),
    ];
    for (options, encrypt, expected) in cases {
        let description = if encrypt {
            error_of(new_encryptor(options, &PRK))
        } else {
            error_of(new_decryptor(options, &PRK))
        };
        assert!(description.starts_with(expected), "{description}");
    }
}

#[test]
#[expect(clippy::float_cmp, reason = "the lengths are exact integers")]
fn encrypted_lengths() {
    assert_eq!(
        encrypted_length(65537.0, "AES-GCM", 65536.0).unwrap(),
        65564.0 + 29.0
    );
    assert!(encrypted_length(-1.0, "AES-GCM", 65536.0).is_err());
    assert!(encrypted_length(9_007_199_254_740_991.0, "AES-GCM", 65536.0).is_err());
    assert!(encrypted_length(1.0, "AES-CTR", 65536.0).is_err());
}

// encryptedLength() accepts exactly the lengths an encryptor does: with 1 KiB segments, 2^32
// segments (4 TiB) are one too many.
#[test]
fn encrypted_length_applies_the_segment_limit() {
    let limit = 4_294_967_295.0 * 1024.0;
    assert!(encrypted_length(limit, "AES-GCM", 1024.0).is_ok());
    assert!(encrypted_length(limit + 1.0, "AES-GCM", 1024.0).is_err());
    let at_limit = |plaintext_length| ffi::CipherOptions {
        segment_size: 1024.0,
        has_plaintext_length: true,
        plaintext_length,
        ..options("AES-GCM")
    };
    assert!(new_encryptor(at_limit(limit), &PRK).is_ok());
    assert!(new_encryptor(at_limit(limit + 1.0), &PRK).is_err());
}

#[test]
fn charges_its_configuration_from_the_start() {
    let big = || ffi::KeyInfo {
        prefix: vec![0; 8192],
        suffix: vec![0; 4096],
        ..info()
    };
    let encryptor = new_encryptor(
        ffi::CipherOptions {
            info: big(),
            ..options("AES-GCM")
        },
        &PRK,
    )
    .unwrap();
    assert!(encryptor.retained_bytes() >= 8192 + 4096 + PRK.len());
    let decryptor = new_decryptor(
        ffi::CipherOptions {
            mac_info: big(),
            ..ffi::CipherOptions {
                has_plaintext_length: true,
                plaintext_length: 1.0,
                ..options("AES-CBC-HMAC-SHA256")
            }
        },
        &PRK,
    )
    .unwrap();
    assert!(decryptor.retained_bytes() >= 8192 + 4096);
}
