// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Trace spans of the current request, from
//! [`IoContext::make_trace_span`](crate::IoContext::make_trace_span) and
//! [`IoContext::make_user_trace_span`](crate::IoContext::make_user_trace_span).
//!
//! Span names and tag keys are `&'static CStr`s, written as `c"..."` literals, which C++ uses
//! without copying, as it does its own string literals.

#![allow(
    unsafe_code,
    reason = "the bridge takes names, keys and literal values that C++ does not copy, which \
              must therefore outlive the span"
)]

use std::ffi::CStr;

use kj_rs::KjOwn;

use crate::bridge;

/// The value of a span tag.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum TagValue<'a> {
    Bool(bool),
    Int(i64),
    Float(f64),
    /// A string, which is copied if the span is recorded.
    Str(&'a str),
    /// A string literal, which is not copied.
    Literal(&'static CStr),
}

impl From<bool> for TagValue<'_> {
    fn from(value: bool) -> Self {
        Self::Bool(value)
    }
}

impl From<i64> for TagValue<'_> {
    fn from(value: i64) -> Self {
        Self::Int(value)
    }
}

impl From<f64> for TagValue<'_> {
    fn from(value: f64) -> Self {
        Self::Float(value)
    }
}

impl<'a> From<&'a str> for TagValue<'a> {
    fn from(value: &'a str) -> Self {
        Self::Str(value)
    }
}

impl<'a> From<&'a String> for TagValue<'a> {
    fn from(value: &'a String) -> Self {
        Self::Str(value)
    }
}

impl From<&'static CStr> for TagValue<'_> {
    fn from(value: &'static CStr) -> Self {
        Self::Literal(value)
    }
}

/// A span being recorded, like C++ `SpanBuilder`, which it holds. The span ends when this is
/// dropped. It is neither `Send` nor `Sync`.
pub struct SpanBuilder(KjOwn<bridge::SpanBuilder>);

impl SpanBuilder {
    /// [`IoContext::make_trace_span`](crate::IoContext::make_trace_span), for the `IoContext`
    /// with id `context_id`.
    pub(crate) fn start(context_id: u64, name: &'static CStr) -> jsg::Result<Self> {
        // SAFETY: `name` is `'static`.
        let span = unsafe { bridge::io_context_make_trace_span(context_id, literal(name)) }?;
        Ok(Self(span))
    }

    /// Sets tag `key` to `value`, replacing any value it had, as C++ `SpanBuilder::setTag()`
    /// does. Does nothing if the span is not recorded.
    pub fn set_tag<'v>(&mut self, key: &'static CStr, value: impl Into<TagValue<'v>>) {
        let (span, key) = (self.0.pin_mut(), literal(key));
        // SAFETY: `key`, and a `Literal` value, are `'static`.
        unsafe {
            match value.into() {
                TagValue::Bool(value) => bridge::span_builder_set_tag_bool(span, key, value),
                TagValue::Int(value) => bridge::span_builder_set_tag_int(span, key, value),
                TagValue::Float(value) => bridge::span_builder_set_tag_float(span, key, value),
                TagValue::Str(value) => bridge::span_builder_set_tag_str(span, key, value),
                TagValue::Literal(value) => {
                    bridge::span_builder_set_tag_literal(span, key, literal(value));
                }
            }
        }
    }

    /// Starts a child span, as C++ `SpanBuilder::newChild()` does.
    #[must_use]
    pub fn new_child(&mut self, name: &'static CStr) -> Self {
        // SAFETY: `name` is `'static`.
        Self(unsafe { bridge::span_builder_new_child(self.0.pin_mut(), literal(name)) })
    }
}

/// A span of both internal and user tracing, like C++ `TraceContext`, which it holds. The spans
/// end when this is dropped. It is neither `Send` nor `Sync`.
pub struct TraceContext(KjOwn<bridge::TraceContext>);

impl TraceContext {
    /// [`IoContext::make_user_trace_span`](crate::IoContext::make_user_trace_span), for the
    /// `IoContext` with id `context_id`.
    pub(crate) fn start(context_id: u64, name: &'static CStr) -> jsg::Result<Self> {
        // SAFETY: `name` is `'static`.
        let context =
            unsafe { bridge::io_context_make_user_trace_span(context_id, literal(name)) }?;
        Ok(Self(context))
    }

    /// Sets tag `key` to `value` on both spans, as C++ `TraceContext::setTag()` does.
    pub fn set_tag<'v>(&mut self, key: &'static CStr, value: impl Into<TagValue<'v>>) {
        let (context, key) = (self.0.pin_mut(), literal(key));
        // SAFETY: `key`, and a `Literal` value, are `'static`.
        unsafe {
            match value.into() {
                TagValue::Bool(value) => bridge::trace_context_set_tag_bool(context, key, value),
                TagValue::Int(value) => bridge::trace_context_set_tag_int(context, key, value),
                TagValue::Float(value) => bridge::trace_context_set_tag_float(context, key, value),
                TagValue::Str(value) => bridge::trace_context_set_tag_str(context, key, value),
                TagValue::Literal(value) => {
                    bridge::trace_context_set_tag_literal(context, key, literal(value));
                }
            }
        }
    }
}

/// Holds no JavaScript handles.
impl jsg::Traced for SpanBuilder {}

/// Holds no JavaScript handles.
impl jsg::Traced for TraceContext {}

/// A literal as the bridge passes it: the bytes of `str` with its NUL.
fn literal(str: &'static CStr) -> &'static [u8] {
    str.to_bytes_with_nul()
}
