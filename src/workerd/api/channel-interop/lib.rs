// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Interop with the C++ JSG resources that stand for `IoChannelFactory` channels, for APIs
//! implemented in Rust: [`Fetcher`], which sends requests to a [`SubrequestChannel`], and
//! [`DurableObjectClass`], a handle on an [`ActorClassChannel`].
//!
//! Rust takes and returns them as `jsg_cpp::Ref<Fetcher>` and `jsg_cpp::Ref<DurableObjectClass>`:
//! method parameters, `#[jsg_struct]` fields, `Option`, `Nullable` and `Vec`s of them accept an
//! instance of the class or of a subclass, such as `ExtendedFetcher`, and reject anything else,
//! as C++ JSG's `jsg::Ref` parameters do (see `jsg_cpp::Ref`). It holds a channel for the current
//! request as a [`ScopedChannel`], and one it keeps past its request, once C++ has allowed its
//! transfer, as the `io_channels` type, which this crate re-exports.
//!
//! The C++ side, `ffi.h` and `ffi.c++`, is the per-type and per-method shims of
//! `src/rust/jsg-cpp`: C++ API code, which lives here with the API rather than in Rust JSG.

mod ffi;

pub use ffi::Channel;
pub use ffi::InHouse;
pub use ffi::RequiresHostAndProtocol;
pub use ffi::ScopedChannel;
pub use ffi::bridge::DurableObjectClass;
pub use ffi::bridge::Fetcher;
pub use io_channels::ActorClassChannel;
pub use io_channels::SubrequestChannel;
