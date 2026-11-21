// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Durable Object containers on a local Docker engine (`Worker.containerEngine.localDocker`).
//!
//! - `registry`: the server's clients, one per container, and their cleanup.
//! - `client`: one container's client, the methods of `rpc::Container`; `exec` and `port` are
//!   the processes and ports it hands out, and `handle` what the C++ shim calls.
//! - `egress`: the mappings a Worker installs and the listener the egress sidecar sends the
//!   container's connections to.
//! - `docker`: HTTP requests to the Docker Engine API and to the egress sidecar, with hyper.
//! - `api`: the JSON bodies of those requests.
//! - `tar`: the one-file archive the API takes a file for a container as.

pub mod api;
pub mod client;
pub mod docker;
pub mod egress;
pub mod exec;
pub mod handle;
pub mod port;
pub mod registry;
pub mod tar;
