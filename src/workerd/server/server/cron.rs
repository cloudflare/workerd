// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Arming cron triggers (`cron/cron.js`). Each Worker that uses the module
//! gets one scheduled event once the server starts, and arms its schedules
//! with it: an object has no client of its own to wake it.

use std::rc::Rc;
use std::time::Duration;
use std::time::SystemTime;

use futures::future::LocalBoxFuture;
use kj_rs::KjMaybe;
use outcome_capnp::EventOutcome;
use worker::CxxWorkerInterface;
use worker::Interface;
use workerd_capnp::config;
use workerd_capnp::service;
use workerd_capnp::worker as worker_capnp;
use workerd_capnp::worker::module;

use crate::Result;
use crate::bridge::ffi;
use crate::channels::Channel;
use crate::config::Server;
use crate::config::capnp_error;
use crate::listen::ListenContext;

const MODULE: &str = "/workerd/cron.js";
/// Attempts, and the delay before the first retry, which doubles each time:
/// about 31 s in all, longer than a cluster's lease takes to expire.
const ATTEMPTS: u32 = 6;
const BACKOFF: Duration = Duration::from_secs(1);

/// The Workers whose main (first) module is the module.
fn cron_workers(config: config::Reader<'_>) -> capnp::Result<Vec<String>> {
    let mut out = Vec::new();
    for s in config.get_services()? {
        let service::Which::Worker(w) = s.which()? else {
            continue;
        };
        let worker_capnp::Which::Modules(modules) = w?.which()? else {
            continue;
        };
        if let Some(m) = modules?.iter().next()
            && let module::Which::Bundled(name) = m.which()?
            && name?.to_str()? == MODULE
        {
            out.push(s.get_name()?.to_string()?);
        }
    }
    Ok(out)
}

/// Arms each cron Worker in the background, each on its own, retried with
/// backoff and then logged; `workerd test` arms none.
///
/// If there is any, a listener that ends with draining: schedules serve until
/// the server drains, sockets or not.
pub fn start(
    context: &ListenContext,
    server: &Server,
    config: config::Reader<'_>,
    for_test: bool,
) -> Result<Option<LocalBoxFuture<'static, Result<()>>>> {
    if for_test {
        return Ok(None);
    }
    let names = cron_workers(config).map_err(capnp_error)?;
    let channels: Vec<_> = server
        .services()
        .filter(|(name, _)| names.iter().any(|n| n == name))
        .map(|(name, s)| (name.to_owned(), s.channel()))
        .collect();
    if channels.is_empty() {
        return Ok(None);
    }
    for (name, channel) in channels {
        let factory_ = Rc::clone(&context.factory);
        context.factory.spawn_detached(async move {
            let mut delay = BACKOFF;
            for attempt in 1..=ATTEMPTS {
                match arm(&*channel).await {
                    Ok(()) => break,
                    Err(e) if attempt == ATTEMPTS => {
                        tracing::error!("cron \"{name}\" not armed: {}", e.description());
                    }
                    Err(e) => {
                        tracing::warn!("cron \"{name}\" not armed, retrying: {}", e.description());
                        factory_.sleep(delay).await;
                        delay *= 2;
                    }
                }
            }
        });
    }
    let mut draining = context.draining.clone();
    Ok(Some(Box::pin(async move {
        let _ = draining.wait_for(|drained| *drained).await;
        Ok(())
    })))
}

async fn arm(channel: &dyn Channel) -> Result<()> {
    let metadata = ffi::new_request_metadata(KjMaybe::None, KjMaybe::None);
    let mut interface = CxxWorkerInterface::new(channel.start_request(metadata)?);
    let result = interface.run_scheduled(&SystemTime::now(), "").await?;
    if result.outcome == EventOutcome::Ok {
        Ok(())
    } else {
        Err(kj::failed!("{:?}", result.outcome))
    }
}
