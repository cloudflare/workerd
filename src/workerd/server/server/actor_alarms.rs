// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A Durable Object namespace's alarms, as the server keeps them.
//!
//! Their times are stored in the namespace's `AlarmStore` as nanoseconds from the Unix epoch.

use std::time::Duration;
use std::time::SystemTime;

/// A time `ns` nanoseconds from the Unix epoch, before it for a negative `ns`.
pub fn time_from_ns(ns: i64) -> SystemTime {
    let offset = Duration::from_nanos(ns.unsigned_abs());
    if ns < 0 {
        SystemTime::UNIX_EPOCH - offset
    } else {
        SystemTime::UNIX_EPOCH + offset
    }
}

/// Nanoseconds from the Unix epoch to `time`, negative before it, saturating.
pub fn time_to_ns(time: SystemTime) -> i64 {
    match time.duration_since(SystemTime::UNIX_EPOCH) {
        Ok(after) => i64::try_from(after.as_nanos()).unwrap_or(i64::MAX),
        Err(before) => i64::try_from(before.duration().as_nanos()).map_or(i64::MIN, |ns| -ns),
    }
}

#[cfg(test)]
#[path = "actor_alarms-test.rs"]
mod tests;
