// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

const IV: [u8; IV_SIZE] = [7; IV_SIZE];

fn key() -> TokenKey {
    TokenKey::from_cluster_key(b"cluster")
}

/// A Frankenvalue holding `json`, as a flat message.
fn json(json: &str) -> Vec<u8> {
    let mut message = message::Builder::new_default();
    message
        .init_root::<frankenvalue::Builder<'_>>()
        .set_json(json);
    serialize::write_message_to_words(&message)
}

fn json_of(flat: &[u8]) -> String {
    let message = read_frankenvalue(flat).unwrap();
    let root = message.get().unwrap();
    match root.which().unwrap() {
        frankenvalue::Json(text) => text.unwrap().to_string().unwrap(),
        _ => panic!("not JSON"),
    }
}

fn service(props: &[u8]) -> Target<'_> {
    Target::Service {
        name: b"foo",
        entrypoint: Some(b"MyEntry"),
        props,
    }
}

#[test]
fn service_tokens_round_trip() {
    let props = json(r#"{"foo": 123}"#);
    for (seal, usage) in [
        (Seal::Rpc { iv: IV }, Usage::Rpc),
        (Seal::Storage, Usage::Storage),
    ] {
        for token_type in [Type::Subrequest, Type::ActorClass] {
            let token =
                encode(&key(), seal, token_type, Persistent::Yes, &service(&props)).unwrap();
            let (persistent, decoded) = decode(&key(), usage, token_type, &token).unwrap();
            assert_eq!(persistent, Persistent::Yes);
            let Decoded::Service {
                name,
                entrypoint,
                props,
            } = decoded
            else {
                panic!("not a service: {decoded:?}");
            };
            assert_eq!(name, b"foo");
            assert_eq!(entrypoint.as_deref(), Some(&b"MyEntry"[..]));
            assert_eq!(json_of(&props.unwrap()), r#"{"foo": 123}"#);
        }
    }
}

#[test]
fn an_absent_entrypoint_stays_absent() {
    let props = json("{}");
    let target = Target::Service {
        name: b"foo",
        entrypoint: None,
        props: &props,
    };
    let token = encode(
        &key(),
        Seal::Rpc { iv: IV },
        Type::Subrequest,
        Persistent::No,
        &target,
    )
    .unwrap();
    let (persistent, decoded) = decode(&key(), Usage::Rpc, Type::Subrequest, &token).unwrap();
    assert_eq!(persistent, Persistent::No);
    assert!(matches!(
        decoded,
        Decoded::Service {
            entrypoint: None,
            ..
        }
    ));
}

#[test]
fn actor_tokens_round_trip() {
    for name in [Some(&b"my-actor"[..]), None] {
        let target = Target::Actor {
            namespace_key: b"foo-namespace",
            id: &[12, 34, 56, 78],
            name,
        };
        let token = encode(
            &key(),
            Seal::Rpc { iv: IV },
            Type::Subrequest,
            Persistent::No,
            &target,
        )
        .unwrap();
        let (_, decoded) = decode(&key(), Usage::Rpc, Type::Subrequest, &token).unwrap();
        assert_eq!(
            decoded,
            Decoded::Actor {
                namespace_key: b"foo-namespace".to_vec(),
                id: vec![12, 34, 56, 78],
                name: name.map(<[u8]>::to_vec),
            }
        );

        for token_type in [Type::ActorClass, Type::Rpc] {
            let message = decode(&key(), Usage::Rpc, token_type, &token)
                .unwrap_err()
                .description()
                .to_owned();
            assert!(message.contains("channel token type mismatch"), "{message}");
        }
    }
}

#[test]
fn restored_tokens_round_trip() {
    let arg = json("[1, 2]");
    let target = Target::Restored {
        vendor: b"vendor token",
        restore_arg: &arg,
    };
    for token_type in [Type::Subrequest, Type::Rpc] {
        let token = encode(&key(), Seal::Storage, token_type, Persistent::Yes, &target).unwrap();
        let (persistent, decoded) = decode(&key(), Usage::Storage, token_type, &token).unwrap();
        assert_eq!(persistent, Persistent::Yes);
        let Decoded::Restored {
            vendor,
            restore_arg,
        } = decoded
        else {
            panic!("not restored: {decoded:?}");
        };
        assert_eq!(vendor, b"vendor token");
        assert_eq!(json_of(&restore_arg.unwrap()), "[1, 2]");
    }
}

#[test]
fn a_type_cannot_point_at_every_target() {
    let props = json("{}");
    let token = encode(
        &key(),
        Seal::Storage,
        Type::Rpc,
        Persistent::Yes,
        &service(&props),
    )
    .unwrap();
    let message = decode(&key(), Usage::Storage, Type::Rpc, &token)
        .unwrap_err()
        .description()
        .to_owned();
    assert!(message.contains("must have a restore chain"), "{message}");

    let target = Target::Restored {
        vendor: b"",
        restore_arg: &props,
    };
    let token = encode(
        &key(),
        Seal::Storage,
        Type::ActorClass,
        Persistent::Yes,
        &target,
    )
    .unwrap();
    let message = decode(&key(), Usage::Storage, Type::ActorClass, &token)
        .unwrap_err()
        .description()
        .to_owned();
    assert!(message.contains("cannot have a restore chain"), "{message}");
}

#[test]
fn storage_tokens_are_the_magic_and_the_packed_message() {
    let props = json("{}");
    let token = encode(
        &key(),
        Seal::Storage,
        Type::Subrequest,
        Persistent::Yes,
        &service(&props),
    )
    .unwrap();
    assert_eq!(token[..4], STORAGE_TOKEN_MAGIC.to_le_bytes());
    let message = serialize_packed::read_message(&mut &token[4..], ReaderOptions::new()).unwrap();
    let root = message.get_root::<channel_token::Reader<'_>>().unwrap();
    assert_eq!(root.get_type().unwrap(), Type::Subrequest);
}

#[test]
fn storage_tokens_are_always_persistent() {
    let props = json("{}");
    let token = encode(
        &key(),
        Seal::Storage,
        Type::Subrequest,
        Persistent::No,
        &service(&props),
    )
    .unwrap();
    let (persistent, _) = decode(&key(), Usage::Storage, Type::Subrequest, &token).unwrap();
    assert_eq!(persistent, Persistent::Yes);
}

#[test]
fn the_envelope_must_match_the_usage() {
    let props = json(r#"{"long enough to pass for an RPC token": true}"#);
    let rpc = encode(
        &key(),
        Seal::Rpc { iv: IV },
        Type::Subrequest,
        Persistent::Yes,
        &service(&props),
    )
    .unwrap();
    let storage = encode(
        &key(),
        Seal::Storage,
        Type::Subrequest,
        Persistent::Yes,
        &service(&props),
    )
    .unwrap();

    let message = decode(&key(), Usage::Storage, Type::Subrequest, &rpc)
        .unwrap_err()
        .description()
        .to_owned();
    assert!(message.contains("STORAGE_TOKEN_MAGIC"), "{message}");
    let message = decode(&key(), Usage::Rpc, Type::Subrequest, &storage)
        .unwrap_err()
        .description()
        .to_owned();
    assert!(message.contains("RPC_TOKEN_MAGIC"), "{message}");
    for len in 0..4 {
        let message = decode(&key(), Usage::Storage, Type::Subrequest, &storage[..len])
            .unwrap_err()
            .description()
            .to_owned();
        assert!(
            message.contains("invalid channel token for storage"),
            "{message}"
        );
    }
}

#[test]
fn a_large_token_spans_segments_and_round_trips() {
    // Past the first segment's 128 words.
    let big = format!("\"{}\"", "x".repeat(4000));
    let props = json(&big);
    let token = encode(
        &key(),
        Seal::Storage,
        Type::Subrequest,
        Persistent::Yes,
        &service(&props),
    )
    .unwrap();
    let (_, decoded) = decode(&key(), Usage::Storage, Type::Subrequest, &token).unwrap();
    let Decoded::Service { props, .. } = decoded else {
        panic!("not a service");
    };
    assert_eq!(json_of(&props.unwrap()), big);
}
