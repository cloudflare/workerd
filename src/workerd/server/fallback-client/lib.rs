// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The client of the module fallback service, a mechanism used only in workerd local development.
//!
//! A worker whose config sets `moduleFallback` (allowed only under `--experimental`) resolves a
//! module specifier that its bundle does not contain by asking the HTTP service at that address.
//! The service answers with the module as JSON (a `Worker.Module` from workerd.capnp, in capnp's
//! JSON encoding), with a 301 redirect whose `Location` is the specifier to resolve instead, or
//! with an error.
//!
//! There are two versions of the request, one per module registry:
//!
//! - V1 (the legacy registry): a `GET` whose query string carries the specifier, the referrer and
//!   the raw specifier as strings, and whose `x-resolve-method` header says how the module is
//!   imported. Import attributes are not sent.
//! - V2 (the new registry): a `POST` whose body is a `FallbackServiceRequest` (workerd.capnp) as
//!   JSON. The specifier and referrer are URLs, and import attributes are included.
//!
//! Module resolution is synchronous, and runs on the server's event loop thread, so the loop
//! cannot be used to wait for the service. Each [`Client`] instead has a [`Worker`] on a thread of
//! its own, with one HTTP/1.1 connection that is reused across requests; the caller blocks until
//! that thread answers. The C++ side is `FallbackServiceClient` (fallback-client.h), which starts
//! the thread.
//!
//! The requests, the reading of responses, the retry and the log lines are those of the C++
//! client this crate replaced, so that services written against it (Miniflare's, vitest-pool-
//! workers') see no difference; [`json`] is capnp's JSON as its C++ codec reads and writes it.

mod client;
mod ffi;
pub mod json;
mod protocol;

pub use client::Client;
pub use client::Worker;
pub use client::new;
pub use protocol::DecodeError;
pub use protocol::HttpRequest;
pub use protocol::ImportType;
pub use protocol::Request;
pub use protocol::Resolution;
pub use protocol::Version;
pub use protocol::decode_module;
pub use protocol::encode;
