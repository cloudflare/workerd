// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The trace data model of `workerd/io/trace.h` in Rust: trace and span ids, trace flags, span
//! contexts and span status, with their text, byte and capnp forms.
//!
//! The C++ types stay, and write and read ids as this crate does: `trace-ids-differential-test`
//! keeps the two in step. Random ids are drawn by C++ only.

mod context;
mod ids;
mod status;

pub use context::InvocationSpanContext;
pub use context::SpanContext;
pub use ids::IdText;
pub use ids::SpanId;
pub use ids::TraceFlags;
pub use ids::TraceId;
pub use ids::format_traceparent;
pub use status::SpanStatus;
pub use status::SpanStatusCode;
