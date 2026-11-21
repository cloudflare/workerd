// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The encoding and crypto of workerd's channel tokens, the bytes that `getToken()` returns for a
//! `SubrequestChannel`, an `ActorClassChannel` or an `RpcChannel` and that `*FromToken()` turns
//! back into a channel (see `ChannelToken` in `channel-token.capnp` for the format).
//!
//! [`TokenKey`] derives the token key and its key ID, and seals and opens the AES-256-GCM envelope
//! of an RPC token. [`encode`] builds a `ChannelToken` message and wraps it for its usage;
//! [`decode`] unwraps and checks a token and returns what it points at.
//!
//! The C++ `ChannelTokenHandler` (`channel-token.h`) calls this through [`ffi`]. It keeps
//! everything that touches live channels: the cap tables of the Frankenvalues in a token, whose
//! entries are themselves tokens of live channels, the restored channels that call
//! `[restore]()`, and the source of the IVs, `workerd::getEntropy()`. A Frankenvalue crosses
//! the bridge as a flat (unpacked, unsegmented-table) capnp message with an `rpc::Frankenvalue`
//! root.

pub mod ffi;
mod key;
mod token;

pub use key::IV_SIZE;
pub use key::KEY_ID_SIZE;
pub use key::KEY_SIZE;
pub use key::TokenKey;
pub use token::Decoded;
pub use token::Persistent;
pub use token::Seal;
pub use token::Target;
pub use token::Usage;
pub use token::decode;
pub use token::encode;

pub type Error = cxx::KjError;
pub type Result<T> = std::result::Result<T, Error>;
