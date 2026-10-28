// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The trace spans of a read, with the names and tags memory-cache.c++ records.

use io_context::IoContext;
use io_context::SpanBuilder;
use io_context::TraceContext;
use memory_cache::EvictionReason;
use memory_cache::ReadTrace;
use memory_cache::WriteOutcome;
use memory_cache::WriteTrace;

use crate::ffi::is_predictable_mode_for_test;

/// The spans of one read, which end when it no longer needs them.
pub struct ReadSpans {
    /// The span of internal tracing, which the read's tags go on.
    pub read: SpanBuilder,
    /// The span of user tracing, which has no tags.
    pub user: TraceContext,
}

impl ReadSpans {
    pub fn start(io_context: &IoContext<'_>) -> jsg::Result<Self> {
        Ok(Self {
            read: io_context.make_trace_span(c"memory_cache_read")?,
            user: io_context.make_user_trace_span(c"memory_cache_read")?,
        })
    }
}

/// Tags a read that the cache answered, with a value or a miss.
pub fn tag_read(span: &mut SpanBuilder, trace: &ReadTrace) {
    span.set_tag(
        c"memory_cache_lock_wait_time_ns",
        lock_wait_ns(trace.lock_wait_ns),
    );
    span.set_tag(c"cache_hit", trace.cache_hit);
    if trace.cache_hit {
        span.set_tag(c"entry_size", count(trace.entry_size));
    }
    tag_cache_size(span, trace);
}

/// Tags a read that missed and runs its fallback.
pub fn tag_leader(span: &mut SpanBuilder, trace: &ReadTrace) {
    span.set_tag(
        c"memory_cache_lock_wait_time_ns",
        lock_wait_ns(trace.lock_wait_ns),
    );
    span.set_tag(c"cache_hit", false);
    span.set_tag(c"coalesced_request", false);
    span.set_tag(c"initiating_fallback", true);
    tag_cache_size(span, trace);
}

/// Tags a read that missed and waits for another's fallback, and starts the span of the wait.
pub fn tag_waiter(span: &mut SpanBuilder, trace: &ReadTrace, key: &str) -> SpanBuilder {
    span.set_tag(
        c"memory_cache_lock_wait_time_ns",
        lock_wait_ns(trace.lock_wait_ns),
    );
    span.set_tag(c"cache_hit", false);
    span.set_tag(c"coalesced_request", true);
    span.set_tag(c"waiting_on_inflight", true);
    span.set_tag(
        c"inflight_waiters_count",
        count(trace.waiters_ahead.saturating_add(1)),
    );
    let mut wait = span.new_child(c"memory_cache_coalesce_wait");
    wait.set_tag(c"key", key);
    wait.set_tag(c"waiters_ahead", count(trace.waiters_ahead));
    wait
}

fn tag_cache_size(span: &mut SpanBuilder, trace: &ReadTrace) {
    span.set_tag(c"cache_total_size", count(trace.total_value_size));
    span.set_tag(c"cache_entry_count", count(trace.entry_count));
}

/// Records the write of what a fallback produced, and the evictions it caused, as spans of
/// their own.
pub fn record_write(io_context: &IoContext<'_>, key: &str, trace: &WriteTrace) -> jsg::Result<()> {
    let mut span = io_context.make_trace_span(c"memory_cache_write")?;
    span.set_tag(c"key", key);
    span.set_tag(c"value_size", count(trace.value_size));
    span.set_tag(c"has_expiration", trace.has_expiration);
    match trace.outcome {
        WriteOutcome::Success {
            is_update,
            total_after,
            entries_after,
        } => {
            span.set_tag(c"write_success", true);
            span.set_tag(c"is_update", is_update);
            span.set_tag(c"evictions_triggered", count(trace.evictions.len()));
            span.set_tag(c"cache_total_size_after", count(total_after));
            span.set_tag(c"cache_entry_count_after", count(entries_after));
        }
        WriteOutcome::ValueTooLarge { max_value_size } => {
            span.set_tag(c"write_rejected", true);
            span.set_tag(c"rejection_reason", c"value_too_large");
            span.set_tag(c"max_value_size", count(max_value_size));
        }
        WriteOutcome::AlreadyExpired => {
            span.set_tag(c"write_rejected", true);
            span.set_tag(c"rejection_reason", c"already_expired");
        }
    }

    for eviction in &trace.evictions {
        let mut span = io_context.make_trace_span(c"memory_cache_eviction")?;
        let reason = match eviction.reason {
            EvictionReason::Expiration => c"expiration",
            EvictionReason::Lru => c"lru",
        };
        span.set_tag(c"eviction_reason", reason);
        span.set_tag(c"evicted_key", &*String::from_utf8_lossy(&eviction.key));
        span.set_tag(c"evicted_size", count(eviction.value_size));
        span.set_tag(c"cache_size_before", count(eviction.total_before));
        span.set_tag(c"cache_entries_before", count(eviction.entries_before));
    }
    Ok(())
}

/// The time a read waited for the cache's lock, which is 0 in tests, so that traces are
/// deterministic.
fn lock_wait_ns(ns: u64) -> i64 {
    if is_predictable_mode_for_test() {
        return 0;
    }
    i64::try_from(ns).unwrap_or(i64::MAX)
}

/// A size or count as a tag, which memory-cache.c++ records as a float.
#[expect(
    clippy::cast_precision_loss,
    reason = "as in memory-cache.c++; sizes and counts of a memory cache are far below 2^53"
)]
pub fn count(n: usize) -> f64 {
    n as f64
}
