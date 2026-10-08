// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Trace timestamps.
//!
//! Timestamps are nanoseconds on the monotonic clock since a process-wide epoch, fixed the first
//! time any trace clock is read. They are precise (not Spectre-coarsened like `IoContext::now()`),
//! which is acceptable only because they never reach JavaScript.

use std::sync::OnceLock;
use std::time::Instant;
use std::time::SystemTime;

/// Nanoseconds since the process trace epoch.
pub type Nanos = u64;

struct Epoch {
    instant: Instant,
    unix_ms: u64,
}

static EPOCH: OnceLock<Epoch> = OnceLock::new();

fn epoch() -> &'static Epoch {
    EPOCH.get_or_init(|| Epoch {
        instant: Instant::now(),
        unix_ms: SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .map_or(0, |d| u64::try_from(d.as_millis()).unwrap_or(u64::MAX)),
    })
}

/// Wall-clock time of the trace epoch, in milliseconds since the Unix epoch. For display only.
#[must_use]
pub fn epoch_unix_ms() -> u64 {
    epoch().unix_ms
}

/// A source of trace timestamps. Production code uses [`MonotonicClock`]; tests substitute a
/// manual clock.
pub trait Clock {
    fn now(&self) -> Nanos;
}

/// The monotonic clock, relative to the process trace epoch.
#[derive(Debug, Clone, Copy, Default)]
pub struct MonotonicClock;

impl Clock for MonotonicClock {
    fn now(&self) -> Nanos {
        let elapsed = Instant::now().saturating_duration_since(epoch().instant);
        u64::try_from(elapsed.as_nanos()).unwrap_or(u64::MAX)
    }
}

#[cfg(test)]
#[path = "clock-test.rs"]
mod tests;
