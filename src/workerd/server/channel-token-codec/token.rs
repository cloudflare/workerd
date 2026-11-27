// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Building and reading the `ChannelToken` message, and wrapping it for its usage.

use capnp::message;
use capnp::message::ReaderOptions;
use capnp::serialize;
use capnp::serialize_packed;
use channel_token_capnp::channel_token;
use channel_token_capnp::channel_token::STORAGE_TOKEN_MAGIC;
use channel_token_capnp::channel_token::Type;
use frankenvalue_capnp::frankenvalue;

use crate::Result;
use crate::key::IV_SIZE;
use crate::key::TokenKey;

/// The words of the first segment of a token's message, as in C++'s `MallocMessageBuilder(128)`,
/// so that a token laid out as C++ lays it out is the same bytes. capnp-rust sizes and fills the
/// segments after the first differently from capnp-cpp, so a token of three or more segments can
/// be different bytes, which read the same.
const FIRST_SEGMENT_WORDS: u32 = 128;

/// What a token is for, which decides its envelope.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Usage {
    /// Sent over RPC: sealed with the token key.
    Rpc,
    /// Kept in long-term storage, such as Durable Object storage: in the clear.
    Storage,
}

/// How to wrap a token: the [`Usage`], with the IV of an RPC token.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Seal {
    Rpc { iv: [u8; IV_SIZE] },
    Storage,
}

impl Seal {
    #[must_use]
    pub fn usage(self) -> Usage {
        match self {
            Self::Rpc { .. } => Usage::Rpc,
            Self::Storage => Usage::Storage,
        }
    }
}

/// Whether the channel may be kept in long-term storage: its target worker allowed irrevocable
/// stub storage (the `persistence` union of `ChannelToken`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Persistent {
    No,
    Yes,
}

/// What a token points at. A Frankenvalue is a flat capnp message whose root is an
/// `rpc::Frankenvalue`, cap table included.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Target<'a> {
    /// An entrypoint of a service (a subrequest or actor class token).
    Service {
        name: &'a [u8],
        entrypoint: Option<&'a [u8]>,
        props: &'a [u8],
    },
    /// An actor (a subrequest token).
    Actor {
        namespace_key: &'a [u8],
        id: &'a [u8],
        name: Option<&'a [u8]>,
    },
    /// The channel that `[restore]()` of `vendor`, a subrequest token, returns for `restore_arg`
    /// (a subrequest or RPC token).
    Restored {
        vendor: &'a [u8],
        restore_arg: &'a [u8],
    },
}

/// A token's contents, as [`decode`] reads them. A Frankenvalue is a flat capnp message whose root
/// is an `rpc::Frankenvalue`, cap table included; `None` if the token has none.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Decoded {
    Service {
        name: Vec<u8>,
        entrypoint: Option<Vec<u8>>,
        props: Option<Vec<u8>>,
    },
    Actor {
        namespace_key: Vec<u8>,
        id: Vec<u8>,
        name: Option<Vec<u8>>,
    },
    Restored {
        vendor: Vec<u8>,
        restore_arg: Option<Vec<u8>>,
    },
}

/// Builds the token of type `token_type` pointing at `target`, wrapped as `seal` says.
///
/// # Errors
///
/// Fails if a Frankenvalue of `target` is not a valid message.
pub fn encode(
    key: &TokenKey,
    seal: Seal,
    token_type: Type,
    persistent: Persistent,
    target: &Target<'_>,
) -> Result<Vec<u8>> {
    let mut message = message::Builder::new(
        message::HeapAllocator::new().first_segment_words(FIRST_SEGMENT_WORDS),
    );
    let mut token = message.init_root::<channel_token::Builder<'_>>();
    token.set_type(token_type);
    match persistent {
        Persistent::No => token.reborrow().init_persistence().set_none(()),
        Persistent::Yes => token.reborrow().init_persistence().set_irrevocable(()),
    }

    match *target {
        Target::Service {
            name,
            entrypoint,
            props,
        } => {
            let props = read_frankenvalue(props)?;
            let mut service = token.init_service();
            service.set_name(capnp::text::Reader::from(name));
            if let Some(entrypoint) = entrypoint {
                service.set_entrypoint(capnp::text::Reader::from(entrypoint));
            }
            copy_frankenvalue(props.get().map_err(capnp_error)?, service.init_props())
                .map_err(capnp_error)?;
        }
        Target::Actor {
            namespace_key,
            id,
            name,
        } => {
            let mut actor = token.init_actor();
            actor.set_namespace_key(capnp::text::Reader::from(namespace_key));
            actor.set_id(id);
            if let Some(name) = name {
                actor.set_name(capnp::text::Reader::from(name));
            }
        }
        Target::Restored {
            vendor,
            restore_arg,
        } => {
            let restore_arg = read_frankenvalue(restore_arg)?;
            let mut restored = token.init_restored();
            restored.set_vendor(vendor);
            copy_frankenvalue(
                restore_arg.get().map_err(capnp_error)?,
                restored.init_restore_arg(),
            )
            .map_err(capnp_error)?;
        }
    }

    let mut plaintext = Vec::new();
    serialize_packed::write_message(&mut plaintext, &message).map_err(capnp_error)?;

    Ok(match seal {
        Seal::Rpc { iv } => key.seal(&iv, &plaintext),
        Seal::Storage => {
            let mut token = Vec::with_capacity(4 + plaintext.len());
            token.extend_from_slice(&STORAGE_TOKEN_MAGIC.to_le_bytes());
            token.extend_from_slice(&plaintext);
            token
        }
    })
}

/// Reads a token of `usage` that must be of type `token_type`, returning whether it is persistent
/// and what it points at.
///
/// A storage token is always persistent: one minted before the `persistence` union existed reads
/// as `none`, but it was stored, so redeeming it still checks that the target worker allows stub
/// storage.
///
/// # Errors
///
/// Fails if the envelope is invalid (see [`TokenKey::open`]), the message is invalid, the token is
/// not of type `token_type`, or `token_type` cannot point at what the token points at.
pub fn decode(
    key: &TokenKey,
    usage: Usage,
    token_type: Type,
    token: &[u8],
) -> Result<(Persistent, Decoded)> {
    let plaintext = match usage {
        Usage::Rpc => key.open(token)?,
        Usage::Storage => {
            if token.len() < 4 {
                kj::fail_require!(
                    "expected token.size() >= sizeof(magic); invalid channel token for storage"
                );
            }
            if token[..4] != STORAGE_TOKEN_MAGIC.to_le_bytes() {
                kj::fail_require!(
                    "expected magic == ChannelToken::STORAGE_TOKEN_MAGIC; invalid channel token \
                     for storage"
                );
            }
            token[4..].to_vec()
        }
    };

    let message = serialize_packed::read_message(&mut plaintext.as_slice(), ReaderOptions::new())
        .map_err(capnp_error)?;
    let reader = message
        .get_root::<channel_token::Reader<'_>>()
        .map_err(capnp_error)?;

    if reader.get_type() != Ok(token_type) {
        kj::fail_require!("expected reader.getType() == type; channel token type mismatch");
    }

    let persistent = match (usage, reader.get_persistence().which()) {
        (Usage::Storage, _) | (Usage::Rpc, Ok(channel_token::persistence::Irrevocable(()))) => {
            Persistent::Yes
        }
        (Usage::Rpc, _) => Persistent::No,
    };

    let decoded = match reader.which() {
        Ok(channel_token::Service(service)) => {
            if token_type == Type::Rpc {
                kj::fail_require!("RPC channel tokens must have a restore chain");
            }
            Decoded::Service {
                name: text(service.get_name())?,
                entrypoint: if service.has_entrypoint() {
                    Some(text(service.get_entrypoint())?)
                } else {
                    None
                },
                props: if service.has_props() {
                    Some(write_frankenvalue(service.get_props())?)
                } else {
                    None
                },
            }
        }
        Ok(channel_token::Actor(actor)) => {
            if token_type != Type::Subrequest {
                kj::fail_require!(
                    "expected type == ChannelToken::Type::SUBREQUEST; channel token type mismatch"
                );
            }
            Decoded::Actor {
                namespace_key: text(actor.get_namespace_key())?,
                id: actor.get_id().map_err(capnp_error)?.to_vec(),
                name: if actor.has_name() {
                    Some(text(actor.get_name())?)
                } else {
                    None
                },
            }
        }
        Ok(channel_token::Restored(restored)) => {
            if token_type == Type::ActorClass {
                kj::fail_require!("actor class channel tokens cannot have a restore chain");
            }
            Decoded::Restored {
                vendor: restored.get_vendor().map_err(capnp_error)?.to_vec(),
                restore_arg: if restored.has_restore_arg() {
                    Some(write_frankenvalue(restored.get_restore_arg())?)
                } else {
                    None
                },
            }
        }
        Err(capnp::NotInSchema(which)) => {
            kj::fail_require!("unknown channel token kind; reader.which() = {which}")
        }
    };

    Ok((persistent, decoded))
}

/// Reads a Frankenvalue, a flat message, that C++ wrote.
fn read_frankenvalue(
    flat: &[u8],
) -> Result<message::TypedReader<serialize::OwnedSegments, frankenvalue::Owned>> {
    let message =
        serialize::read_message(&mut &*flat, ReaderOptions::new()).map_err(capnp_error)?;
    Ok(message::TypedReader::new(message))
}

/// Copies a Frankenvalue into a token in the order that C++ writes one in place: the value, the cap
/// table size and the properties, each property's name before its value, as
/// `Frankenvalue::toCapnp()` does, then the cap table. A capnp deep copy, which follows the order
/// of the pointer fields, would lay out a value with properties differently, and so seal it into
/// different bytes, though it would read the same.
fn copy_frankenvalue(
    from: frankenvalue::Reader<'_>,
    mut to: frankenvalue::Builder<'_>,
) -> capnp::Result<()> {
    copy_frankenvalue_value(from, to.reborrow())?;
    if from.has_name() {
        to.set_name(from.get_name()?);
    }
    if from.has_cap_table() {
        to.get_cap_table().set_as(
            from.get_cap_table()
                .get_as::<channel_token::frankenvalue_cap_table::Reader<'_>>()?,
        )?;
    }
    Ok(())
}

/// Copies all of a Frankenvalue but its name and its cap table.
fn copy_frankenvalue_value(
    from: frankenvalue::Reader<'_>,
    mut to: frankenvalue::Builder<'_>,
) -> capnp::Result<()> {
    match from.which()? {
        frankenvalue::EmptyObject(()) => to.set_empty_object(()),
        frankenvalue::Json(json) => to.set_json(json?),
        frankenvalue::V8Serialized(data) => to.set_v8_serialized(data?),
        frankenvalue::ArrayBuffer(data) => to.set_array_buffer(data?),
        frankenvalue::Capability(capability) => {
            let mut to = to.reborrow().init_capability();
            to.set_cap_index(capability.get_cap_index());
            to.set_tag(capability.get_tag());
            if capability.has_wrapper_module() {
                to.set_wrapper_module(capability.get_wrapper_module()?);
            }
        }
    }
    to.set_cap_table_size(from.get_cap_table_size());
    if from.has_properties() {
        let properties = from.get_properties()?;
        let mut list = to.init_properties(properties.len());
        for (i, property) in (0..).zip(properties.iter()) {
            let mut element = list.reborrow().get(i);
            if property.has_name() {
                element.set_name(property.get_name()?);
            }
            copy_frankenvalue_value(property, element)?;
        }
    }
    Ok(())
}

/// Copies a Frankenvalue of a token into a flat message of its own, for C++.
fn write_frankenvalue(value: capnp::Result<frankenvalue::Reader<'_>>) -> Result<Vec<u8>> {
    let mut message = message::Builder::new_default();
    message
        .set_root(value.map_err(capnp_error)?)
        .map_err(capnp_error)?;
    Ok(serialize::write_message_to_words(&message))
}

/// The bytes of a text field. Text crosses to C++ as bytes, unchecked, as C++ reads it.
fn text(value: capnp::Result<capnp::text::Reader<'_>>) -> Result<Vec<u8>> {
    Ok(value.map_err(capnp_error)?.as_bytes().to_vec())
}

#[expect(
    clippy::needless_pass_by_value,
    reason = "used as `map_err(capnp_error)`"
)]
fn capnp_error(error: capnp::Error) -> crate::Error {
    kj::failed!("invalid channel token: {}", error)
}

#[cfg(test)]
#[path = "token-test.rs"]
mod tests;
