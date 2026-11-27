// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn a_secret_key_is_32_bytes() {
    assert!(new_token_key(&[0; 32]).is_ok());
    assert!(new_token_key(&[0; 31]).is_err());
}

#[test]
fn an_rpc_iv_is_12_bytes() {
    let key = new_cluster_token_key(b"cluster");
    let encode = |usage, iv: &[u8]| {
        encode_actor_token(
            &key,
            usage,
            iv,
            TokenPersistence::No,
            b"ns",
            b"id",
            b"",
            false,
        )
    };
    assert!(encode(TokenUsage::Rpc, &[0; 12]).is_ok());
    assert!(encode(TokenUsage::Rpc, &[0; 11]).is_err());
    assert!(encode(TokenUsage::Storage, &[]).is_ok());
}

#[test]
fn decode_reports_an_actor() {
    let key = new_cluster_token_key(b"cluster");
    let token = encode_actor_token(
        &key,
        TokenUsage::Storage,
        &[],
        TokenPersistence::Yes,
        b"ns",
        &[1, 2],
        b"name",
        true,
    )
    .unwrap();
    let decoded = decode_token(&key, TokenUsage::Storage, TokenType::Subrequest, &token).unwrap();
    assert_eq!(decoded.kind, DecodedKind::Actor);
    assert_eq!(decoded.persistent, TokenPersistence::Yes);
    assert_eq!(decoded.name, b"ns");
    assert_eq!(decoded.id, [1, 2]);
    assert_eq!(decoded.entrypoint, b"name");
    assert!(decoded.has_entrypoint);
    assert!(!decoded.has_value);
}
