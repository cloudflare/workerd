// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;
use crate::schedule::IndexEncoding;
use crate::schedule::Secret;

/// Deterministic bytes, so that tests don't depend on a random source.
#[derive(Default)]
pub struct CountingEntropy(u8);

impl Entropy for CountingEntropy {
    fn fill(&mut self, out: &mut [u8]) -> Result<()> {
        for byte in out {
            *byte = self.0;
            self.0 = self.0.wrapping_add(1);
        }
        Ok(())
    }
}

pub fn info(index: IndexEncoding) -> InfoTemplate {
    InfoTemplate {
        prefix: b"segment ".to_vec(),
        index,
        suffix: Vec::new(),
    }
}

pub fn cipher(algorithm: Algorithm, segment_size: u64) -> Cipher {
    let mac_info = (algorithm == Algorithm::AesCbcHmacSha256).then(|| InfoTemplate {
        prefix: b"mac ".to_vec(),
        index: IndexEncoding::Uint32Be,
        suffix: Vec::new(),
    });
    Cipher::new(
        Layout::new(algorithm, segment_size).unwrap(),
        KeySchedule {
            digest: ffi::Digest::Sha256,
            prk: Secret::new(vec![0x42; 32]),
        },
        info(IndexEncoding::Decimal),
        mac_info,
        256,
    )
    .unwrap()
}

const ALGORITHMS: [Algorithm; 3] = [
    Algorithm::AesGcm,
    Algorithm::ChaCha20Poly1305,
    Algorithm::AesCbcHmacSha256,
];

#[test]
fn round_trip() {
    for algorithm in ALGORITHMS {
        let cipher = cipher(algorithm, 1024);
        // Sizes on and around the CBC block boundaries.
        for size in [1, 15, 16, 17, 1023, 1024] {
            let plaintext: Vec<u8> = (0..size).map(|i| (i % 251) as u8).collect();
            let mut sealed = b"before".to_vec();
            cipher
                .seal(7, &plaintext, &mut CountingEntropy::default(), &mut sealed)
                .unwrap();
            assert_eq!(sealed.len(), 6 + cipher.layout().sealed_segment_size(size));
            let mut opened = b"before".to_vec();
            assert_eq!(cipher.open(7, &sealed[6..], &mut opened).unwrap(), size);
            assert_eq!(&opened[6..], plaintext.as_slice(), "{algorithm:?} {size}");
        }
    }
}

#[test]
fn rejects_tampering_and_moved_segments() {
    for algorithm in ALGORITHMS {
        let cipher = cipher(algorithm, 1024);
        let mut sealed = Vec::new();
        cipher
            .seal(3, &[1; 100], &mut CountingEntropy::default(), &mut sealed)
            .unwrap();
        let mut out = b"kept".to_vec();
        // At another index.
        assert!(cipher.open(4, &sealed, &mut out).is_err());
        // With each byte flipped in turn.
        for i in 0..sealed.len() {
            let mut tampered = sealed.clone();
            tampered[i] ^= 1;
            assert!(
                cipher.open(3, &tampered, &mut out).is_err(),
                "{algorithm:?} {i}"
            );
        }
        // Truncated.
        assert!(
            cipher
                .open(3, &sealed[..sealed.len() - 1], &mut out)
                .is_err()
        );
        assert!(cipher.open(3, &[], &mut out).is_err());
        assert_eq!(out, b"kept");
    }
}

#[test]
fn key_schedule_must_suit_the_algorithm() {
    let keys = || KeySchedule {
        digest: ffi::Digest::Sha256,
        prk: Secret::new(vec![1; 32]),
    };
    let layout = |algorithm| Layout::new(algorithm, 1024).unwrap();
    let gcm = |bits| {
        Cipher::new(
            layout(Algorithm::AesGcm),
            keys(),
            info(IndexEncoding::Decimal),
            None,
            bits,
        )
    };
    assert!(gcm(128).is_ok());
    assert!(gcm(192).is_err());
    assert!(
        Cipher::new(
            layout(Algorithm::ChaCha20Poly1305),
            keys(),
            info(IndexEncoding::Decimal),
            Some(info(IndexEncoding::Decimal)),
            256,
        )
        .is_err()
    );
    assert!(
        Cipher::new(
            layout(Algorithm::AesCbcHmacSha256),
            keys(),
            info(IndexEncoding::Decimal),
            None,
            256,
        )
        .is_err()
    );
}

#[test]
fn max_index_is_the_smaller_encoding() {
    assert_eq!(cipher(Algorithm::AesGcm, 1024).max_index(), u64::MAX);
    assert_eq!(
        cipher(Algorithm::AesCbcHmacSha256, 1024).max_index(),
        u64::from(u32::MAX)
    );
}
