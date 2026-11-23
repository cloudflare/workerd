// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn sizes() {
    let gcm = Layout::new(Algorithm::AesGcm, 65536).unwrap();
    assert_eq!(gcm.sealed_length(0), 0);
    assert_eq!(gcm.sealed_length(65536), 65564);
    assert_eq!(gcm.sealed_length(65537), 65564 + 29);
    assert_eq!(gcm.segment_count(65537), 2);
    let cbc = Layout::new(Algorithm::AesCbcHmacSha256, 65536).unwrap();
    assert_eq!(cbc.sealed_length(65536), 65600);
    assert_eq!(cbc.sealed_length(15), 16 + 16 + 32);
    assert_eq!(cbc.sealed_length(16), 16 + 32 + 32);
    assert_eq!(cbc.segment_plaintext_size(65537, 1), 1);
    assert_eq!(cbc.segment_plaintext_size(65537, 2), 0);
}

#[test]
fn segment_size_range() {
    assert!(Layout::new(Algorithm::AesGcm, 1023).is_err());
    assert!(Layout::new(Algorithm::AesGcm, 1024).is_ok());
    assert!(Layout::new(Algorithm::AesGcm, 16 * 1024 * 1024).is_ok());
    assert!(Layout::new(Algorithm::AesGcm, 16 * 1024 * 1024 + 1).is_err());
    assert!(Algorithm::parse("AES-CTR").is_err());
}
