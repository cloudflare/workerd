// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge through which `FallbackServiceClient` (fallback-client.h) uses a [`Client`] and
//! runs its [`Worker`] on a `kj::Thread`, which installs the creating thread's logger (the JSON
//! logger, under structured logging) around it.
//!
//! Specifiers, referrers, attributes and redirects cross as bytes: a `kj::StringPtr` need not be
//! UTF-8.

#![allow(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]

use crate::client;
use crate::client::Client;
use crate::client::Worker;
use crate::protocol;

#[cxx::bridge(namespace = "workerd::fallback")]
mod bridge {
    /// `Version`, for C++.
    enum Version {
        V1,
        V2,
    }

    /// `ImportType`, for C++.
    enum ImportType {
        Import,
        Require,
        Internal,
    }

    /// An import attribute.
    struct Attribute {
        name: Vec<u8>,
        value: Vec<u8>,
    }

    /// Which `Resolution`.
    enum ResolutionKind {
        NotFound,
        Redirect,
        Module,
        ShutDown,
    }

    /// `Resolution`, for C++: `bytes` is the specifier of a `Redirect` and the capnp message of a
    /// `Module`, and empty otherwise.
    struct Resolution {
        kind: ResolutionKind,
        bytes: Vec<u8>,
    }

    /// A new client and the worker that must run on a thread of its own for it to answer.
    struct NewClient {
        client: Box<Client>,
        worker: Box<Worker>,
    }

    extern "Rust" {
        #[cxx_name = "FallbackClient"]
        type Client;
        #[cxx_name = "FallbackWorker"]
        type Worker;

        /// A client of the fallback service at `address`, and its worker.
        #[cxx_name = "newFallbackClient"]
        fn new_client(address: &[u8]) -> NewClient;

        /// Runs `worker` on the calling thread until its client is dropped.
        #[cxx_name = "runFallbackWorker"]
        fn run_worker(worker: Box<Worker>);

        /// Asks the service for a module, blocking until the worker answers. An empty
        /// `raw_specifier` is one that is not known.
        #[cxx_name = "resolve"]
        fn resolve_ffi(
            self: &Client,
            version: Version,
            import_type: ImportType,
            specifier: &[u8],
            raw_specifier: &[u8],
            referrer: &[u8],
            attributes: &[Attribute],
        ) -> Resolution;
    }
}

fn new_client(address: &[u8]) -> bridge::NewClient {
    let (client, worker) = client::new(String::from_utf8_lossy(address).into_owned());
    bridge::NewClient {
        client: Box::new(client),
        worker: Box::new(worker),
    }
}

#[expect(clippy::boxed_local, reason = "cxx hands the worker over as a Box")]
fn run_worker(worker: Box<Worker>) {
    worker.run();
}

impl Client {
    fn resolve_ffi(
        &self,
        version: bridge::Version,
        import_type: bridge::ImportType,
        specifier: &[u8],
        raw_specifier: &[u8],
        referrer: &[u8],
        attributes: &[bridge::Attribute],
    ) -> bridge::Resolution {
        let version = match version {
            bridge::Version::V2 => protocol::Version::V2,
            _ => protocol::Version::V1,
        };
        let import_type = match import_type {
            bridge::ImportType::Require => protocol::ImportType::Require,
            bridge::ImportType::Internal => protocol::ImportType::Internal,
            _ => protocol::ImportType::Import,
        };
        let request = protocol::Request {
            version,
            import_type,
            specifier: specifier.to_vec(),
            raw_specifier: (!raw_specifier.is_empty()).then(|| raw_specifier.to_vec()),
            referrer: referrer.to_vec(),
            attributes: attributes
                .iter()
                .map(|a| (a.name.clone(), a.value.clone()))
                .collect(),
        };
        let (kind, bytes) = match self.resolve(request) {
            protocol::Resolution::NotFound => (bridge::ResolutionKind::NotFound, Vec::new()),
            protocol::Resolution::Redirect(specifier) => {
                (bridge::ResolutionKind::Redirect, specifier)
            }
            protocol::Resolution::Module(module) => (bridge::ResolutionKind::Module, module),
            protocol::Resolution::ShutDown => (bridge::ResolutionKind::ShutDown, Vec::new()),
        };
        bridge::Resolution { kind, bytes }
    }
}
