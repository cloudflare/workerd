// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The C++ interface, used by `ChannelTokenHandler` (`channel-token.h`).
//!
//! cxx has no `Option` for a shared struct or a slice, so an optional value crosses as a value
//! and a `has_*` flag; text crosses as bytes.

use channel_token_capnp::channel_token::Type;

use crate::Decoded;
use crate::Persistent;
use crate::Result;
use crate::Seal;
use crate::Target;
use crate::TokenKey;
use crate::Usage;

#[cxx::bridge(namespace = "workerd::server::channel_token_codec")]
#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
#[expect(
    clippy::too_many_arguments,
    reason = "the bridge declarations take the fields of a token"
)]
mod bridge {
    /// `ChannelToken::Type`, with the same values.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    #[repr(u16)]
    enum TokenType {
        Subrequest = 0,
        ActorClass = 1,
        Rpc = 2,
    }

    /// `IoChannelFactory::ChannelTokenUsage`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum TokenUsage {
        Rpc,
        Storage,
    }

    /// `Persistent`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum TokenPersistence {
        No,
        Yes,
    }

    /// The union of a decoded token.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum DecodedKind {
        Service,
        Actor,
        Restored,
    }

    /// A decoded token. A Frankenvalue (`value`) is a flat capnp message with an
    /// `rpc::Frankenvalue` root.
    #[derive(Debug)]
    struct DecodedToken {
        kind: DecodedKind,
        persistent: TokenPersistence,
        /// A service's name, or an actor's namespace key.
        name: Vec<u8>,
        /// A service's entrypoint, or an actor's name.
        entrypoint: Vec<u8>,
        has_entrypoint: bool,
        /// An actor's ID.
        id: Vec<u8>,
        /// A restored token's vendor token.
        vendor: Vec<u8>,
        /// A service's props, or a restored token's restore argument.
        value: Vec<u8>,
        has_value: bool,
    }

    extern "Rust" {
        type TokenKey;

        /// A key from 32 random bytes, so that only this process redeems its tokens.
        fn new_token_key(secret: &[u8]) -> Result<Box<TokenKey>>;

        /// The key derived from a cluster's key, shared by the instances of the cluster.
        fn new_cluster_token_key(cluster_key: &[u8]) -> Box<TokenKey>;

        /// The token of an entrypoint of a service. `iv` (12 bytes) is the IV of an RPC token,
        /// unused for storage.
        fn encode_service_token(
            key: &TokenKey,
            usage: TokenUsage,
            iv: &[u8],
            token_type: TokenType,
            persistent: TokenPersistence,
            name: &[u8],
            entrypoint: &[u8],
            has_entrypoint: bool,
            props: &[u8],
        ) -> Result<Vec<u8>>;

        /// The token of an actor.
        fn encode_actor_token(
            key: &TokenKey,
            usage: TokenUsage,
            iv: &[u8],
            persistent: TokenPersistence,
            namespace_key: &[u8],
            id: &[u8],
            name: &[u8],
            has_name: bool,
        ) -> Result<Vec<u8>>;

        /// The token of what `[restore]()` of `vendor` returns for `restore_arg`.
        fn encode_restored_token(
            key: &TokenKey,
            usage: TokenUsage,
            iv: &[u8],
            token_type: TokenType,
            persistent: TokenPersistence,
            vendor: &[u8],
            restore_arg: &[u8],
        ) -> Result<Vec<u8>>;

        /// Reads a token that must be of type `token_type`.
        fn decode_token(
            key: &TokenKey,
            usage: TokenUsage,
            token_type: TokenType,
            token: &[u8],
        ) -> Result<DecodedToken>;
    }
}

pub use bridge::DecodedKind;
pub use bridge::DecodedToken;
pub use bridge::TokenPersistence;
pub use bridge::TokenType;
pub use bridge::TokenUsage;

/// Creates the key of a process for C++.
///
/// # Errors
///
/// Fails if `secret` is not 32 bytes.
pub fn new_token_key(secret: &[u8]) -> Result<Box<TokenKey>> {
    let Ok(secret) = secret.try_into() else {
        kj::fail_require!("a channel token key is 32 bytes, not {}", secret.len());
    };
    Ok(Box::new(TokenKey::from_secret(secret)))
}

/// Creates the key of a cluster for C++.
#[must_use]
pub fn new_cluster_token_key(cluster_key: &[u8]) -> Box<TokenKey> {
    Box::new(TokenKey::from_cluster_key(cluster_key))
}

/// Encodes a service token for C++.
///
/// # Errors
///
/// See [`crate::encode`]; also fails if the IV of an RPC token is not 12 bytes.
#[expect(clippy::too_many_arguments, reason = "the fields of a service token")]
pub fn encode_service_token(
    key: &TokenKey,
    usage: TokenUsage,
    iv: &[u8],
    token_type: TokenType,
    persistent: TokenPersistence,
    name: &[u8],
    entrypoint: &[u8],
    has_entrypoint: bool,
    props: &[u8],
) -> Result<Vec<u8>> {
    let target = Target::Service {
        name,
        entrypoint: has_entrypoint.then_some(entrypoint),
        props,
    };
    crate::encode(
        key,
        seal(usage, iv)?,
        token_type.try_into()?,
        persistent.try_into()?,
        &target,
    )
}

/// Encodes an actor token for C++.
///
/// # Errors
///
/// See [`crate::encode`]; also fails if the IV of an RPC token is not 12 bytes.
#[expect(clippy::too_many_arguments, reason = "the fields of an actor token")]
pub fn encode_actor_token(
    key: &TokenKey,
    usage: TokenUsage,
    iv: &[u8],
    persistent: TokenPersistence,
    namespace_key: &[u8],
    id: &[u8],
    name: &[u8],
    has_name: bool,
) -> Result<Vec<u8>> {
    let target = Target::Actor {
        namespace_key,
        id,
        name: has_name.then_some(name),
    };
    crate::encode(
        key,
        seal(usage, iv)?,
        Type::Subrequest,
        persistent.try_into()?,
        &target,
    )
}

/// Encodes a restored token for C++.
///
/// # Errors
///
/// See [`crate::encode`]; also fails if the IV of an RPC token is not 12 bytes.
pub fn encode_restored_token(
    key: &TokenKey,
    usage: TokenUsage,
    iv: &[u8],
    token_type: TokenType,
    persistent: TokenPersistence,
    vendor: &[u8],
    restore_arg: &[u8],
) -> Result<Vec<u8>> {
    let target = Target::Restored {
        vendor,
        restore_arg,
    };
    crate::encode(
        key,
        seal(usage, iv)?,
        token_type.try_into()?,
        persistent.try_into()?,
        &target,
    )
}

/// Decodes a token for C++.
///
/// # Errors
///
/// See [`crate::decode`].
pub fn decode_token(
    key: &TokenKey,
    usage: TokenUsage,
    token_type: TokenType,
    token: &[u8],
) -> Result<DecodedToken> {
    let (persistent, decoded) =
        crate::decode(key, usage.try_into()?, token_type.try_into()?, token)?;
    let mut result = DecodedToken {
        kind: DecodedKind::Service,
        persistent: match persistent {
            Persistent::No => TokenPersistence::No,
            Persistent::Yes => TokenPersistence::Yes,
        },
        name: Vec::new(),
        entrypoint: Vec::new(),
        has_entrypoint: false,
        id: Vec::new(),
        vendor: Vec::new(),
        value: Vec::new(),
        has_value: false,
    };
    match decoded {
        Decoded::Service {
            name,
            entrypoint,
            props,
        } => {
            result.name = name;
            (result.entrypoint, result.has_entrypoint) = split_option(entrypoint);
            (result.value, result.has_value) = split_option(props);
        }
        Decoded::Actor {
            namespace_key,
            id,
            name,
        } => {
            result.kind = DecodedKind::Actor;
            result.name = namespace_key;
            result.id = id;
            (result.entrypoint, result.has_entrypoint) = split_option(name);
        }
        Decoded::Restored {
            vendor,
            restore_arg,
        } => {
            result.kind = DecodedKind::Restored;
            result.vendor = vendor;
            (result.value, result.has_value) = split_option(restore_arg);
        }
    }
    Ok(result)
}

fn split_option(value: Option<Vec<u8>>) -> (Vec<u8>, bool) {
    match value {
        Some(value) => (value, true),
        None => (Vec::new(), false),
    }
}

fn seal(usage: TokenUsage, iv: &[u8]) -> Result<Seal> {
    match usage.try_into()? {
        Usage::Rpc => {
            let Ok(iv) = iv.try_into() else {
                kj::fail_require!("a channel token IV is 12 bytes, not {}", iv.len());
            };
            Ok(Seal::Rpc { iv })
        }
        Usage::Storage => Ok(Seal::Storage),
    }
}

impl TryFrom<TokenType> for Type {
    type Error = crate::Error;

    fn try_from(value: TokenType) -> Result<Self> {
        match value {
            TokenType::Subrequest => Ok(Self::Subrequest),
            TokenType::ActorClass => Ok(Self::ActorClass),
            TokenType::Rpc => Ok(Self::Rpc),
            _ => kj::fail_require!("unknown channel token type {}", value.repr),
        }
    }
}

impl TryFrom<TokenUsage> for Usage {
    type Error = crate::Error;

    fn try_from(value: TokenUsage) -> Result<Self> {
        match value {
            TokenUsage::Rpc => Ok(Self::Rpc),
            TokenUsage::Storage => Ok(Self::Storage),
            _ => kj::fail_require!("unknown channel token usage {}", value.repr),
        }
    }
}

impl TryFrom<TokenPersistence> for Persistent {
    type Error = crate::Error;

    fn try_from(value: TokenPersistence) -> Result<Self> {
        match value {
            TokenPersistence::No => Ok(Self::No),
            TokenPersistence::Yes => Ok(Self::Yes),
            _ => kj::fail_require!("unknown channel token persistence {}", value.repr),
        }
    }
}

#[cfg(test)]
#[path = "ffi-test.rs"]
mod tests;
