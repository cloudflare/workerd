// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `AnalyticsEngine`, the JavaScript API of a Workers Analytics Engine binding. It does what
//! `src/workerd/api/analytics-engine.c++` does.
//!
//! Each data point is written to a logfwdr channel as an `AnalyticsEngineEvent` message
//! (`src/workerd/api/analytics-engine.capnp`).

pub mod data_point;
pub mod encode;
mod ffi;

use std::time::UNIX_EPOCH;

use io_context::IoContext;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

use crate::data_point::DataPoint;
use crate::encode::Header;

/// What each error a data point over a limit throws starts with.
const ERROR_PREFIX: &str = "writeDataPoint(): ";

#[jsg_resource(name = "AnalyticsEngine")]
pub struct AnalyticsEngine {
    binding: Binding,
}

/// What an `AnalyticsEngine` writes to: a logfwdr channel, and what each of its messages starts
/// with.
#[jsg_macros::jsg_traced]
#[derive(Debug, Clone)]
pub struct Binding {
    pub channel: u32,
    pub header: Header,
}

impl AnalyticsEngine {
    #[must_use]
    pub fn new(binding: Binding) -> Self {
        Self { binding }
    }
}

// A binding transfers through a startup snapshot as its channel and header, as the C++ one does
// through snapshotClone().
impl jsg::SnapshotTransfer for AnalyticsEngine {
    type Snapshot = Binding;

    fn snapshot(&self) -> Binding {
        self.binding.clone()
    }

    fn restore(binding: &Binding) -> Self {
        Self::new(binding.clone())
    }
}

#[jsg_resource]
impl AnalyticsEngine {
    /// `writeDataPoint(event?)`: writes a data point, making no delivery guarantees. One over a
    /// limit throws. In a Durable Object, it is written once the output gate opens, and its
    /// timestamp is the time it is written.
    #[jsg_method]
    fn write_data_point(
        &self,
        lock: &mut jsg::Lock<'_>,
        event: Option<DataPoint>,
    ) -> jsg::Result<()> {
        let context = IoContext::current(lock)?;
        context.new_analytics_engine_request()?;
        let point = event.unwrap_or_default();
        encode::validate(&point, ERROR_PREFIX)?;
        let header = self.binding.header.clone();
        context.write_logfwdr_with(self.binding.channel, move || {
            encode::encode(&header, &point, timestamp())
        })
    }
}

/// The time, in nanoseconds since the Unix epoch, as C++ `AnalyticsEngine::now()` computes it:
/// the request's clock in whole milliseconds, converted to nanoseconds in double arithmetic, so
/// not exact above 2^53. Outside a request, it is 0.
fn timestamp() -> i64 {
    let millis = IoContext::now_in_current_request()
        .and_then(|now| now.duration_since(UNIX_EPOCH).ok())
        .map_or(0, |since| since.as_millis());
    #[expect(
        clippy::cast_precision_loss,
        clippy::cast_possible_truncation,
        reason = "C++ converts the milliseconds to a double, and the product back to an integer"
    )]
    let nanos = (millis as f64 * 1e6) as i64;
    nanos
}
