// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]

//! The bridge that declares the C++ channel types.
//!
//! The bridges of other crates alias them: `type SubrequestChannel =
//! io_channels::ffi::SubrequestChannel;` under `#[namespace = "workerd::io_channels"]`, with
//! `include!("workerd/io/io-channels/ffi.h")`.

pub use bridge::ActorClassChannel;
pub use bridge::RpcChannel;
pub use bridge::SubrequestChannel;

#[cxx::bridge(namespace = "workerd::io_channels")]
mod bridge {
    unsafe extern "C++" {
        include!("workerd/io/io-channels/ffi.h");

        /// C++ `IoChannelFactory::SubrequestChannel`: somewhere requests can be sent, such as a
        /// service, an actor or the network.
        type SubrequestChannel;

        /// C++ `IoChannelFactory::ActorClassChannel`: a Durable Object class.
        type ActorClassChannel;

        /// C++ `IoChannelFactory::RpcChannel`: a persistent JS RPC target of another worker,
        /// which can be saved as a channel token.
        type RpcChannel;
    }
}
