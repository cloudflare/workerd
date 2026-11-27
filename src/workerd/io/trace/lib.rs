// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The trace data model of `workerd/io/trace.h` in Rust: trace and span ids, trace flags, span
//! contexts, span status, tag values, attributes, span links and the event infos of an onset, with
//! their text, byte and capnp forms.
//!
//! The C++ types stay, and write and read these as this crate does:
//! `trace-types-differential-test` keeps the two in step. Random ids are drawn by C++ only.

mod attribute;
mod context;
mod event;
mod ids;
mod link;
mod status;

pub use attribute::Attribute;
pub use attribute::TagValue;
pub use context::InvocationSpanContext;
pub use context::SpanContext;
pub use event::AlarmEventInfo;
pub use event::EmailEventInfo;
pub use event::EventInfo;
pub use event::FetchEventInfo;
pub use event::FetchHeader;
pub use event::FetchResponseInfo;
pub use event::HibernatableWebSocketEventInfo;
pub use event::HttpMethod;
pub use event::JsRpcEventInfo;
pub use event::OnHibernateEventInfo;
pub use event::OnHibernateReason;
pub use event::QueueEventInfo;
pub use event::ScheduledEventInfo;
pub use event::TraceEventInfo;
pub use event::TraceItem;
pub use event::TracePreview;
pub use ids::IdText;
pub use ids::SpanId;
pub use ids::TraceFlags;
pub use ids::TraceId;
pub use ids::format_traceparent;
pub use link::SpanLink;
pub use link::SpanLinks;
pub use status::SpanStatus;
pub use status::SpanStatusCode;
