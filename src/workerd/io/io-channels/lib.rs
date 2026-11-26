// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The channels of C++ `IoChannelFactory` (`workerd/io/io-channels.h`) for Rust:
//! [`SubrequestChannel`], [`ActorClassChannel`] and [`RpcChannel`].
//!
//! Each is a newtype over a `kj_rs::KjRc` of the C++ object, a `kj::Rc` whose clones share its
//! count: a channel that crosses into Rust and back into C++ is the same C++ object. Rust code
//! holds and passes the newtypes; a bridge declares a function over the C++ type, aliased from
//! [`ffi`], and converts with `from_kj` and `into_kj` at the call, or borrows with `as_kj`.
//!
//! A channel belongs to the thread it was made on, so the newtypes are neither `Send` nor `Sync`.
//! It may be dropped with or without the isolate lock, and after the `IoContext` it was made in is
//! gone.

pub mod ffi;

use kj_rs::KjRc;

/// C++ `IoChannelFactory::SubrequestChannel`: somewhere requests can be sent.
#[derive(Clone)]
pub struct SubrequestChannel(KjRc<ffi::SubrequestChannel>);

/// C++ `IoChannelFactory::ActorClassChannel`: a Durable Object class.
#[derive(Clone)]
pub struct ActorClassChannel(KjRc<ffi::ActorClassChannel>);

/// C++ `IoChannelFactory::RpcChannel`: a persistent JS RPC target of another worker, which can be
/// saved as a channel token.
#[derive(Clone)]
pub struct RpcChannel(KjRc<ffi::RpcChannel>);

macro_rules! kj_conversions {
    ($name:ident) => {
        impl $name {
            /// The channel `channel` is a reference to.
            #[must_use]
            pub const fn from_kj(channel: KjRc<ffi::$name>) -> Self {
                Self(channel)
            }

            /// The reference to the C++ channel, for a bridge function that takes one.
            #[must_use]
            pub fn into_kj(self) -> KjRc<ffi::$name> {
                self.0
            }

            /// The reference to the C++ channel, for a bridge function that borrows one.
            #[must_use]
            pub const fn as_kj(&self) -> &KjRc<ffi::$name> {
                &self.0
            }
        }
    };
}

kj_conversions!(SubrequestChannel);
kj_conversions!(ActorClassChannel);
kj_conversions!(RpcChannel);
