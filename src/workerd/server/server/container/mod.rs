// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Durable Object containers on a local Docker engine (`Worker.containerEngine.localDocker`).
//!
//! - `docker`: HTTP requests to the Docker Engine API and to the egress sidecar, with hyper.
//! - `api`: the JSON bodies of those requests.
//! - `tar`: the one-file archive the API takes a file for a container as.

pub mod api;
pub mod docker;
pub mod tar;
