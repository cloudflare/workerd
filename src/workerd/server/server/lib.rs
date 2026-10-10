// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! workerd's server: the single-tenant Workers runtime.
//!
//! The server reads the config (workerd.capnp), builds the service graph, binds the sockets, and
//! runs the event loop. Everything that touches the isolate is delegated to the C++ worker
//! factory (worker-factory.h) through the bridge in `bridge`; the server owns the rest:
//! config interpretation, bindings and channel numbering, services, listeners, actors, dynamic
//! workers, drain and the test runner.

pub mod actor;
pub mod bindings;
pub mod bridge;
pub mod channels;
pub mod config;
pub mod entry;
pub mod in_process;
pub mod listen;
pub mod loader;
pub mod log;
pub mod run;
pub mod services;
pub mod tasks;
pub mod worker;

pub type Error = cxx::KjError;
pub type Result<T> = std::result::Result<T, Error>;
