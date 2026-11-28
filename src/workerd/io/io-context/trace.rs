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
use std::pin::Pin;

use kj_rs::KjOwn;
use kj_rs::LoopChecked;

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

impl From<i32> for TagValue<'_> {
    fn from(value: i32) -> Self {
        Self::Int(value.into())
    }
}

impl From<u32> for TagValue<'_> {
    fn from(value: u32) -> Self {
        Self::Int(value.into())
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
/// dropped.
///
/// It belongs to the event loop of the request that started it. It is `Send`, so that a `Send`
/// future or callback of the request can hold it, but not `Sync`. Using it off its loop panics,
/// and dropping it off its loop aborts the process, as for a [`LoopChecked`].
pub struct SpanBuilder(LoopChecked<KjOwn<bridge::SpanBuilder>>);

impl SpanBuilder {
    /// [`IoContext::make_trace_span`](crate::IoContext::make_trace_span), for the `IoContext`
    /// with id `context_id`.
    pub(crate) fn start(context_id: u64, name: &'static CStr) -> jsg::Result<Self> {
        // SAFETY: `name` is `'static`.
        let span = unsafe { bridge::io_context_make_trace_span(context_id, literal(name)) }?;
        // The bridge made the span in the current `IoContext`, so on its event loop.
        Ok(Self::on_current_loop(span))
    }

    /// Wraps `span`, which was made on the current event loop.
    fn on_current_loop(span: KjOwn<bridge::SpanBuilder>) -> Self {
        // SAFETY: a C++ `SpanBuilder` belongs to the event loop of its request: it shares its
        // observer only with the request's other spans, through a non-atomic refcount, and neither
        // it nor its observer keeps thread-local state across a return to the event loop.
        Self(unsafe { LoopChecked::new(span) })
    }

    /// The C++ span.
    ///
    /// # Panics
    ///
    /// Panics off the span's event loop.
    #[expect(
        clippy::panic,
        reason = "using a span off its event loop is a bug in the caller"
    )]
    fn span(&mut self) -> Pin<&mut bridge::SpanBuilder> {
        match self.0.get_mut() {
            Ok(span) => span.pin_mut(),
            Err(err) => panic!("{}", err.what()),
        }
    }

    /// Sets tag `key` to `value`, as C++ `SpanBuilder::setTag()` does. Does nothing if the span
    /// is not recorded. Set each tag at most once: as in C++, setting one again replaces its
    /// value, but is treated as a bug, logged as a warning, and as an error in predictable mode.
    ///
    /// # Panics
    ///
    /// Panics off the span's event loop.
    pub fn set_tag<'v>(&mut self, key: &'static CStr, value: impl Into<TagValue<'v>>) {
        let (span, key) = (self.span(), literal(key));
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
    ///
    /// # Panics
    ///
    /// Panics off the span's event loop.
    #[must_use]
    pub fn new_child(&mut self, name: &'static CStr) -> Self {
        // SAFETY: `name` is `'static`.
        let child = unsafe { bridge::span_builder_new_child(self.span(), literal(name)) };
        // `span()` checked that the current event loop is this span's, and so the child's.
        Self::on_current_loop(child)
    }
}

/// A span of both internal and user tracing, like C++ `TraceContext`, which it holds. The spans
/// end when this is dropped.
///
/// It belongs to the event loop of its request, as a [`SpanBuilder`] does: it is `Send` but not
/// `Sync`, using it off its loop panics, and dropping it off its loop aborts the process.
pub struct TraceContext(LoopChecked<KjOwn<bridge::TraceContext>>);

impl TraceContext {
    /// [`IoContext::make_user_trace_span`](crate::IoContext::make_user_trace_span), for the
    /// `IoContext` with id `context_id`.
    pub(crate) fn start(context_id: u64, name: &'static CStr) -> jsg::Result<Self> {
        // SAFETY: `name` is `'static`.
        let context =
            unsafe { bridge::io_context_make_user_trace_span(context_id, literal(name)) }?;
        // SAFETY: the bridge made the spans in the current `IoContext`, so on its event loop, and
        // they are as for `SpanBuilder::on_current_loop()`. Their async trace operation belongs to
        // an `AsyncTracker`, which skips calls from any OS thread but the one it was made on:
        // after its loop moves, the operation is not recorded, which is sound.
        Ok(Self(unsafe { LoopChecked::new(context) }))
    }

    /// The C++ spans.
    ///
    /// # Panics
    ///
    /// Panics off the spans' event loop.
    #[expect(
        clippy::panic,
        reason = "using a span off its event loop is a bug in the caller"
    )]
    fn context(&mut self) -> Pin<&mut bridge::TraceContext> {
        match self.0.get_mut() {
            Ok(context) => context.pin_mut(),
            Err(err) => panic!("{}", err.what()),
        }
    }

    /// Sets tag `key` to `value` on both spans, as C++ `TraceContext::setTag()` does. If the
    /// request has async tracing, it also annotates the span's operation, even if the spans are
    /// not recorded. Set each tag at most once, as for [`SpanBuilder::set_tag`].
    ///
    /// # Panics
    ///
    /// Panics off the spans' event loop.
    pub fn set_tag<'v>(&mut self, key: &'static CStr, value: impl Into<TagValue<'v>>) {
        let (context, key) = (self.context(), literal(key));
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

    /// Stops the next [`IoContext::await_io_and_run`](crate::IoContext::await_io_and_run) in this
    /// turn from reporting its wait under the span's operation, as C++
    /// `TraceContext::detachAsync()` does. Call it when this outlives the current call without
    /// being moved into the callback of the next `await_io_and_run()`, such as when it is stored
    /// for later; otherwise an unrelated wait later in the turn could take over the operation.
    ///
    /// # Panics
    ///
    /// Panics off the spans' event loop.
    pub fn detach_async(&mut self) {
        bridge::trace_context_detach_async(self.context());
    }
}

/// A literal as the bridge passes it: the bytes of `str` with its NUL.
fn literal(str: &'static CStr) -> &'static [u8] {
    str.to_bytes_with_nul()
}
