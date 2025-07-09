// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust types corresponding to C++ WorkerInterface types

// No direct FFI usage in this file
use std::future::Future;
use std::pin::Pin;

/// Outcome of an event for logging/metrics purposes
///
/// C++ equivalent: `EventOutcome` enum from `workerd/io/outcome.capnp.h`
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub enum EventOutcome {
    Unknown = 0,
    Ok = 1,
    Exception = 2,
    ExceededMemory = 3,
    ExceededCpu = 4,
    Canceled = 5,
}

/// Result of a scheduled event
///
/// C++ equivalent: `workerd::WorkerInterface::ScheduledResult`
/// ```c++
/// struct ScheduledResult {
///   bool retry = true;
///   EventOutcome outcome = EventOutcome::UNKNOWN;
/// };
/// ```
#[derive(Debug, Clone)]
pub struct ScheduledResult {
    pub retry: bool,
    pub outcome: EventOutcome,
}

impl Default for ScheduledResult {
    fn default() -> Self {
        Self {
            retry: true,
            outcome: EventOutcome::Unknown,
        }
    }
}

/// Result of an alarm event
///
/// C++ equivalent: `workerd::WorkerInterface::AlarmResult`
/// ```c++
/// struct AlarmResult {
///   bool retry = true;
///   bool retryCountsAgainstLimit = true;
///   EventOutcome outcome = EventOutcome::UNKNOWN;
/// };
/// ```
#[derive(Debug, Clone)]
pub struct AlarmResult {
    pub retry: bool,
    pub retry_counts_against_limit: bool,
    pub outcome: EventOutcome,
}

impl Default for AlarmResult {
    fn default() -> Self {
        Self {
            retry: true,
            retry_counts_against_limit: true,
            outcome: EventOutcome::Unknown,
        }
    }
}

/// Result of a custom event
///
/// C++ equivalent: `workerd::WorkerInterface::CustomEvent::Result`
/// ```c++
/// struct Result {
///   EventOutcome outcome;  // Outcome for logging / metrics purposes.
/// };
/// ```
#[derive(Debug, Clone)]
pub struct CustomEventResult {
    pub outcome: EventOutcome,
}

impl Default for CustomEventResult {
    fn default() -> Self {
        Self {
            outcome: EventOutcome::Unknown,
        }
    }
}

/// Constants for alarm retry behavior
///
/// C++ equivalent:
/// ```c++
/// static constexpr auto ALARM_RETRY_START_SECONDS = 2;  // not a duration so we can left shift it
/// static constexpr auto ALARM_RETRY_MAX_TRIES = 6;
/// ```
///
/// These constants are shared by multiple systems that invoke alarms (the production
/// implementation, and the preview implementation), whose code live in completely different
/// places. We end up defining them here mostly for lack of a better option.
pub const ALARM_RETRY_START_SECONDS: u32 = 2;
pub const ALARM_RETRY_MAX_TRIES: u32 = 6;

/// Boxed async future type alias for convenience
pub type AsyncResult<T> = Pin<Box<dyn Future<Output = Result<T, Box<dyn std::error::Error>>>>>;

/// Boxed async future type alias for void results
pub type AsyncVoidResult = Pin<Box<dyn Future<Output = Result<(), Box<dyn std::error::Error>>>>>;
