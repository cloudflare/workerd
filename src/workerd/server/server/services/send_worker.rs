// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A send worker (`send_worker::Worker`) as a service of the graph. Experimental: no config
//! makes one; a binary registers it in code (`crate::native`).
//!
//! The service is the worker and the channels its subrequests reach. Each event is a fresh
//! `send_worker::Adapter`, an ordinary `WorkerInterface` for whoever started the request, with
//! the worker's futures on the send worker runtime.
//!
//! A request's metadata (the cf blob, the client address, what caused the request) does not
//! reach the worker, and a subrequest of the worker carries none.

use std::rc::Rc;
use std::sync::Arc;

use kj::http::HeaderTable;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use send_worker::Adapter;
use send_worker::Host;
use send_worker::Worker;
use tokio::runtime::Handle;
use worker::Interface;

use crate::Result;
use crate::bridge::ffi;
use crate::channels::Channel;
use crate::channels::PendingToken;
use crate::channels::RequestMetadata;
use crate::channels::TokenUsage;
use crate::channels::WorkerInterface;
use crate::config::Factory;
use crate::services::header_table;
use crate::services::not_transferable;

/// What the worker's adapters reach the server through.
struct ServerHost {
    factory: Rc<Factory>,
    /// The worker's subrequest channels, by number.
    channels: Vec<Rc<dyn Channel>>,
}

impl Host for ServerHost {
    fn header_table(&self) -> &HeaderTable {
        header_table(&self.factory)
    }

    fn start_subrequest(&self, channel: u32) -> Result<KjOwn<WorkerInterface>> {
        let target = usize::try_from(channel)
            .ok()
            .and_then(|channel| self.channels.get(channel))
            .ok_or_else(|| kj::failed!("a send worker has no subrequest channel {channel}"))?;
        target.start_request(ffi::new_request_metadata(KjMaybe::None, KjMaybe::None))
    }
}

/// A send worker as a service.
pub struct SendWorkerService {
    worker: Arc<dyn Worker>,
    runtime: Handle,
    host: Rc<ServerHost>,
}

impl SendWorkerService {
    /// `worker`, with its futures on `runtime` and subrequest channel `n` reaching
    /// `channels[n]`.
    #[must_use]
    pub fn new(
        factory: &Rc<Factory>,
        worker: Arc<dyn Worker>,
        runtime: Handle,
        channels: Vec<Rc<dyn Channel>>,
    ) -> Self {
        Self {
            worker,
            runtime,
            host: Rc::new(ServerHost {
                factory: Rc::clone(factory),
                channels,
            }),
        }
    }
}

impl Channel for SendWorkerService {
    fn start_request(&self, _metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        let host: Rc<dyn Host> = Rc::clone(&self.host) as _;
        Ok(Adapter::new(Arc::clone(&self.worker), self.runtime.clone(), host).into_kj())
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("A send worker"))
    }
}
