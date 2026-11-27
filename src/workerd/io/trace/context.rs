// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The span contexts of `workerd/io/trace.h`: `SpanContext`, which tail events carry,
//! `InvocationSpanContext`, which every invocation has, and the W3C `traceparent` header.

use worker_interface_capnp::invocation_span_context;
use worker_interface_capnp::span_context;
use worker_interface_capnp::trace_flags;
use worker_interface_capnp::trace_id;

use crate::IdText;
use crate::SpanId;
use crate::TraceFlags;
use crate::TraceId;
use crate::format_traceparent;
use crate::ids::parse_hex64;

impl TraceId {
    pub fn from_capnp(reader: trace_id::Reader<'_>) -> Self {
        Self {
            high: reader.get_high(),
            low: reader.get_low(),
        }
    }

    pub fn to_capnp(self, mut builder: trace_id::Builder<'_>) {
        builder.set_low(self.low);
        builder.set_high(self.high);
    }
}

/// The flags of a `TraceFlags` field, if it is present and set.
fn flags_from_capnp(present: bool, reader: trace_flags::Reader<'_>) -> Option<TraceFlags> {
    if !present {
        return None;
    }
    match reader.get_value().which() {
        Ok(trace_flags::value::Set(flags)) => Some(TraceFlags(flags)),
        Ok(trace_flags::value::Unset(())) | Err(_) => None,
    }
}

/// The span context of a tail event, `tracing::SpanContext`. `span_id` is `None` only for an
/// onset that inherits no context from another invocation.
#[derive(Clone, Copy, Debug)]
pub struct SpanContext {
    pub trace_id: TraceId,
    pub span_id: Option<SpanId>,
    /// From an upstream `traceparent`; `None` when upstream made no sampling decision.
    pub trace_flags: Option<TraceFlags>,
}

/// Compares the ids only, as C++ does.
impl PartialEq for SpanContext {
    fn eq(&self, other: &Self) -> bool {
        self.trace_id == other.trace_id && self.span_id == other.span_id
    }
}

impl SpanContext {
    pub fn from_capnp(reader: span_context::Reader<'_>) -> capnp::Result<Self> {
        // An unknown variant reads as no span, as in C++.
        let span_id = match reader.get_info().which() {
            Ok(span_context::info::SpanId(id)) => Some(SpanId(id)),
            Ok(span_context::info::Empty(())) | Err(_) => None,
        };
        Ok(Self {
            trace_id: TraceId::from_capnp(reader.get_trace_id()?),
            span_id,
            trace_flags: flags_from_capnp(reader.has_trace_flags(), reader.get_trace_flags()?),
        })
    }

    pub fn to_capnp(&self, mut builder: span_context::Builder<'_>) {
        self.trace_id.to_capnp(builder.reborrow().init_trace_id());
        let mut info = builder.reborrow().init_info();
        match self.span_id {
            Some(id) => info.set_span_id(id.0),
            None => info.set_empty(()),
        }
        if let Some(flags) = self.trace_flags {
            builder.init_trace_flags().init_value().set_set(flags.0);
        }
    }

    /// Parses a W3C `traceparent` header. Only version 00 is accepted, in its fixed-width form,
    /// and neither id may be zero; the hex digits may be in either case.
    ///
    /// Reference: <https://www.w3.org/TR/trace-context/#traceparent-header>
    pub fn from_traceparent(tp: &[u8]) -> Option<Self> {
        // {version:2}-{trace-id:32}-{parent-id:16}-{flags:2}
        if tp.len() != 55 || tp[2] != b'-' || tp[35] != b'-' || tp[52] != b'-' {
            return None;
        }
        let version = parse_hex64(&tp[0..2])?;
        let trace_id = TraceId::from_w3c_text(&tp[3..35])?;
        let span_id = SpanId::from_w3c_text(&tp[36..52])?;
        let flags = parse_hex64(&tp[53..55])?;
        if version != 0 || trace_id.is_null() || span_id.is_null() {
            return None;
        }
        Some(Self {
            trace_id,
            span_id: Some(span_id),
            trace_flags: Some(TraceFlags(u8::try_from(flags).ok()?)),
        })
    }

    /// The `traceparent` header of this context, if it has a span id and flags and neither id
    /// is zero.
    pub fn to_traceparent(&self) -> Option<IdText> {
        let span_id = self.span_id?;
        let flags = self.trace_flags?;
        if self.trace_id.is_null() || span_id.is_null() {
            return None;
        }
        Some(format_traceparent(self.trace_id, span_id, flags))
    }
}

/// The ids of an invocation, `tracing::InvocationSpanContext`.
///
/// They are the trace it belongs to, the invocation, and a span within it. `parent` is the
/// context of the span it was created under, or of the invocation that triggered it.
#[derive(Clone, Debug)]
pub struct InvocationSpanContext {
    pub trace_id: TraceId,
    pub invocation_id: TraceId,
    pub span_id: SpanId,
    pub trace_flags: Option<TraceFlags>,
    pub parent: Option<Box<Self>>,
    /// Read from capnp: a context propagated from another invocation, which C++ won't create
    /// children of. C++ marks these by having no entropy source.
    pub is_trigger: bool,
}

/// Compares the ids only, as C++ does.
impl PartialEq for InvocationSpanContext {
    fn eq(&self, other: &Self) -> bool {
        self.trace_id == other.trace_id
            && self.invocation_id == other.invocation_id
            && self.span_id == other.span_id
    }
}

impl InvocationSpanContext {
    /// Supplies a local sampling decision without overriding one propagated from upstream.
    pub fn set_default_trace_flags(&mut self, flags: TraceFlags) {
        self.trace_flags.get_or_insert(flags);
    }

    /// A trigger context read from capnp, or `None` if either 128-bit id is absent or zero.
    pub fn from_capnp(reader: invocation_span_context::Reader<'_>) -> capnp::Result<Option<Self>> {
        if !reader.has_trace_id() || !reader.has_invocation_id() {
            return Ok(None);
        }
        let trace_id = TraceId::from_capnp(reader.get_trace_id()?);
        let invocation_id = TraceId::from_capnp(reader.get_invocation_id()?);
        if trace_id.is_null() || invocation_id.is_null() {
            return Ok(None);
        }
        Ok(Some(Self {
            trace_id,
            invocation_id,
            span_id: SpanId(reader.get_span_id()),
            trace_flags: flags_from_capnp(reader.has_trace_flags(), reader.get_trace_flags()?),
            parent: None,
            is_trigger: true,
        }))
    }

    pub fn to_capnp(&self, mut builder: invocation_span_context::Builder<'_>) {
        self.trace_id.to_capnp(builder.reborrow().init_trace_id());
        self.invocation_id
            .to_capnp(builder.reborrow().init_invocation_id());
        builder.set_span_id(self.span_id.0);
        if let Some(flags) = self.trace_flags {
            builder.init_trace_flags().init_value().set_set(flags.0);
        }
    }
}

#[cfg(test)]
#[path = "context-test.rs"]
mod tests;
