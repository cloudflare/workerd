// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
#![expect(clippy::unnecessary_box_returns, reason = "cxx requires a Box")]

//! The Rust half of `io-channels-test.c++`: functions that take each channel from C++ as its
//! newtype and hand it back as Rust code does, through `into_kj` or a clone's `as_kj`, and a
//! holder of a channel and its clone.

use std::any::Any;

use io_channels::ActorClassChannel;
use io_channels::RpcChannel;
use io_channels::SubrequestChannel;
use kj_rs::KjRc;

#[cxx::bridge(namespace = "workerd::io_channels::test")]
mod ffi {
    #[namespace = "workerd::io_channels"]
    unsafe extern "C++" {
        include!("workerd/io/io-channels/ffi.h");
        type SubrequestChannel = io_channels::ffi::SubrequestChannel;
        type ActorClassChannel = io_channels::ffi::ActorClassChannel;
        type RpcChannel = io_channels::ffi::RpcChannel;
    }

    extern "Rust" {
        /// Channels that Rust holds; C++ drops them one at a time.
        type Held;
        fn release_one(self: &mut Held);

        /// `channel` through `from_kj`, then `into_kj`.
        fn subrequest_into_kj(channel: KjRc<SubrequestChannel>) -> KjRc<SubrequestChannel>;
        /// `channel` through `from_kj`, then a clone's `as_kj`, the newtype dropped meanwhile.
        fn subrequest_as_kj(channel: KjRc<SubrequestChannel>) -> KjRc<SubrequestChannel>;
        /// `channel` through `from_kj`, held with a clone of the newtype.
        fn subrequest_hold(channel: KjRc<SubrequestChannel>) -> Box<Held>;

        fn actor_class_into_kj(channel: KjRc<ActorClassChannel>) -> KjRc<ActorClassChannel>;
        fn actor_class_as_kj(channel: KjRc<ActorClassChannel>) -> KjRc<ActorClassChannel>;
        fn actor_class_hold(channel: KjRc<ActorClassChannel>) -> Box<Held>;

        fn rpc_into_kj(channel: KjRc<RpcChannel>) -> KjRc<RpcChannel>;
        fn rpc_as_kj(channel: KjRc<RpcChannel>) -> KjRc<RpcChannel>;
        fn rpc_hold(channel: KjRc<RpcChannel>) -> Box<Held>;
    }
}

pub struct Held(Vec<Box<dyn Any>>);

impl Held {
    fn release_one(&mut self) {
        self.0.pop();
    }
}

macro_rules! round_trips {
    ($name:ident, $into_kj:ident, $as_kj:ident, $hold:ident) => {
        fn $into_kj(channel: KjRc<io_channels::ffi::$name>) -> KjRc<io_channels::ffi::$name> {
            $name::from_kj(channel).into_kj()
        }

        fn $as_kj(channel: KjRc<io_channels::ffi::$name>) -> KjRc<io_channels::ffi::$name> {
            let channel = $name::from_kj(channel);
            channel.clone().as_kj().clone()
        }

        fn $hold(channel: KjRc<io_channels::ffi::$name>) -> Box<Held> {
            let channel = $name::from_kj(channel);
            let clone = channel.clone();
            Box::new(Held(vec![Box::new(channel), Box::new(clone)]))
        }
    };
}

round_trips!(
    SubrequestChannel,
    subrequest_into_kj,
    subrequest_as_kj,
    subrequest_hold
);
round_trips!(
    ActorClassChannel,
    actor_class_into_kj,
    actor_class_as_kj,
    actor_class_hold
);
round_trips!(RpcChannel, rpc_into_kj, rpc_as_kj, rpc_hold);
