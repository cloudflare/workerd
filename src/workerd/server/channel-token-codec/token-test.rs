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

/// A token of golden-tokens.txt.
struct GoldenToken {
    name: &'static str,
    usage: Usage,
    token_type: Type,
    /// Whether the codec writes the same bytes from the same contents.
    same_layout: bool,
    token: Vec<u8>,
}

fn golden_tokens() -> Vec<GoldenToken> {
    include_str!("golden-tokens.txt")
        .lines()
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
        .map(|line| {
            let [name, usage, token_type, layout, hex] = line.split(' ').collect::<Vec<_>>()[..]
            else {
                panic!("bad line: {line}");
            };
            let usage = match usage {
                "rpc" => Usage::Rpc,
                "storage" => Usage::Storage,
                _ => panic!("bad usage: {line}"),
            };
            let token_type = match token_type {
                "Subrequest" => Type::Subrequest,
                "ActorClass" => Type::ActorClass,
                "Rpc" => Type::Rpc,
                _ => panic!("bad type: {line}"),
            };
            let same_layout = match layout {
                "same" => true,
                "different" => false,
                _ => panic!("bad layout: {line}"),
            };
            let token = (0..hex.len())
                .step_by(2)
                .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap())
                .collect();
            GoldenToken {
                name,
                usage,
                token_type,
                same_layout,
                token,
            }
        })
        .collect()
}

fn golden(name: &str) -> (Usage, Type, Vec<u8>) {
    let golden = golden_tokens()
        .into_iter()
        .find(|golden| golden.name == name)
        .unwrap();
    (golden.usage, golden.token_type, golden.token)
}

/// The number of segments of a token's message.
fn segments(key: &TokenKey, usage: Usage, token: &[u8]) -> usize {
    let plaintext = match usage {
        Usage::Rpc => key.open(token).unwrap(),
        Usage::Storage => token[4..].to_vec(),
    };
    let segments = serialize_packed::read_message(&mut plaintext.as_slice(), ReaderOptions::new())
        .unwrap()
        .into_segments();
    capnp::message::ReaderSegments::len(&segments)
}

#[test]
fn golden_tokens_encode_to_the_same_contents_and_mostly_the_same_bytes() {
    let key = TokenKey::from_cluster_key(b"golden-cluster-key");
    let tokens = golden_tokens();
    assert_eq!(tokens.len(), 45);
    for golden in tokens {
        let name = golden.name;
        let (persistent, decoded) =
            decode(&key, golden.usage, golden.token_type, &golden.token).unwrap();
        let seal = match golden.usage {
            Usage::Rpc => Seal::Rpc {
                iv: golden.token[4..16].try_into().unwrap(),
            },
            Usage::Storage => Seal::Storage,
        };
        let target = match &decoded {
            Decoded::Service {
                name,
                entrypoint,
                props,
            } => Target::Service {
                name,
                entrypoint: entrypoint.as_deref(),
                props: props.as_deref().unwrap(),
            },
            Decoded::Actor {
                namespace_key,
                id,
                name,
            } => Target::Actor {
                namespace_key,
                id,
                name: name.as_deref(),
            },
            Decoded::Restored {
                vendor,
                restore_arg,
            } => Target::Restored {
                vendor,
                restore_arg: restore_arg.as_deref().unwrap(),
            },
        };
        let again = encode(&key, seal, golden.token_type, persistent, &target).unwrap();

        if golden.same_layout {
            assert_eq!(again, golden.token, "{name}");
        } else {
            assert_ne!(again, golden.token, "{name} has the same layout");
            assert!(
                segments(&key, golden.usage, &golden.token) >= 3,
                "{name}: a different layout of fewer than three segments"
            );
            assert_eq!(
                decode(&key, golden.usage, golden.token_type, &again).unwrap(),
                (persistent, decoded),
                "{name}"
            );
        }
    }
}

#[test]
fn golden_tokens_read_as_written() {
    let key = TokenKey::from_cluster_key(b"golden-cluster-key");
    for usage in ["rpc", "storage"] {
        let (usage, token_type, token) = golden(&format!("{usage}-subrequest-json"));
        let (persistent, decoded) = decode(&key, usage, token_type, &token).unwrap();
        assert_eq!(persistent, Persistent::Yes);
        let Decoded::Service {
            name,
            entrypoint,
            props,
        } = decoded
        else {
            panic!("not a service");
        };
        assert_eq!(name, b"my-service");
        assert_eq!(entrypoint.as_deref(), Some(&b"MyEntry"[..]));
        assert_eq!(json_of(&props.unwrap()), r#"{"foo": 123}"#);
    }

    let (usage, token_type, token) = golden("rpc-actor-not-persistent");
    let (persistent, decoded) = decode(&key, usage, token_type, &token).unwrap();
    assert_eq!(persistent, Persistent::No);
    assert_eq!(
        decoded,
        Decoded::Actor {
            namespace_key: b"my-namespace".to_vec(),
            id: vec![0xde, 0xad, 0xbe, 0xef],
            name: Some(b"my-actor".to_vec()),
        }
    );

    let (usage, token_type, token) = golden("storage-restored-subrequest");
    let (_, decoded) = decode(&key, usage, token_type, &token).unwrap();
    let Decoded::Restored {
        vendor,
        restore_arg,
    } = decoded
    else {
        panic!("not restored");
    };
    assert_eq!(vendor, golden("storage-vendor").2);
    assert_eq!(json_of(&restore_arg.unwrap()), r#"{"restore": "me"}"#);

    // Another key fails to authenticate a golden RPC token.
    let (usage, token_type, token) = golden("rpc-subrequest-json");
    let other = TokenKey::from_cluster_key(b"another-cluster-key");
    assert!(decode(&other, usage, token_type, &token).is_err());
}
