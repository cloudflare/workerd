// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "holds a cxx bridge, which expands to unsafe FFI glue"
)]

//! The C++ interface. C++ owns every object here through `rust::Box`.
//!
//! Kinds and outcomes cross as `u8`, with the values of [`Kind::from_u8`] and
//! [`Outcome::from_u8`]. A resource or stack ID of `0` means "none" in both directions.
//!
//! Strings from C++ cross as bytes and are decoded lossily, because cxx's `rust::Str` constructor
//! throws on invalid UTF-8 and tracing must never throw. Valid UTF-8 is not copied.

use std::borrow::Cow;
use std::io;
use std::pin::Pin;
use std::sync::Arc;

use kj_rs::KjOwn;

use crate::AsyncId;
use crate::ContextId;
use crate::ContextInfo;
use crate::ContextStats;
use crate::Frame;
use crate::InitEvent;
use crate::IsolateId;
use crate::IsolateState;
use crate::Kind;
use crate::MonotonicClock;
use crate::Nanos;
use crate::NdjsonSink;
use crate::NdjsonWriter;
use crate::Outcome;
use crate::Sink;
use crate::StackId;
use crate::Tracker;
use crate::Turn;

#[cxx::bridge(namespace = "workerd::rust::async_trace")]
mod bridge {
    extern "Rust" {
        /// Per-isolate state, shared by the isolate's trackers.
        type Isolate;
        /// One per traced `IoContext`. Used only on the context's thread.
        type Tracker;
        /// A process-wide NDJSON output file.
        type Writer;

        #[expect(
            clippy::unnecessary_box_returns,
            reason = "c++ expects heap-allocation"
        )]
        fn new_isolate() -> Box<Isolate>;

        /// `actor` is empty for a non-actor context.
        fn new_tracker(isolate: &Isolate, worker: &[u8], actor: &[u8]) -> Box<Tracker>;

        /// Creates (or truncates) `path` and writes the header. Fails if `path` is not UTF-8.
        fn open_ndjson_writer(path: &[u8], producer_version: &[u8]) -> Result<Box<Writer>>;
        fn failed(self: &Writer) -> bool;
        /// Flushes buffered lines to the file.
        fn flush(self: &Writer);

        fn add_ndjson_sink(self: &mut Tracker, writer: &Writer);
        fn add_cpp_sink(self: &mut Tracker, listener: KjOwn<AsyncTraceListener>);

        #[cxx_name = "create"]
        fn create_ffi(self: &mut Tracker, kind: u8, name: &[u8], trigger: u64, stack: u32) -> u64;
        #[cxx_name = "settle"]
        fn settle_ffi(self: &mut Tracker, id: u64, outcome: u8);
        fn destroy(self: &mut Tracker, id: u64);
        #[cxx_name = "annotate"]
        fn annotate_ffi(self: &mut Tracker, id: u64, key: &[u8], value: &[u8]);
        fn enter(self: &mut Tracker, id: u64);
        fn exit(self: &mut Tracker, id: u64);
        fn current(self: &Tracker) -> u64;

        fn turn_begin(self: &mut Tracker, default_cause: u64);
        fn turn_locked(self: &mut Tracker);
        fn set_turn_cause(self: &mut Tracker, id: u64);
        fn turn_end(self: &mut Tracker);

        fn adopt_operation(self: &mut Tracker) -> u64;
        fn mark_bound(self: &mut Tracker, id: u64);
        fn count_foreign_thread(self: &mut Tracker, count: u64);

        fn accepts_resources(self: &Tracker) -> bool;
        fn begin_stack(self: &mut Tracker);
        #[cxx_name = "push_frame"]
        fn push_frame_ffi(
            self: &mut Tracker,
            function: &[u8],
            script: &[u8],
            script_id: i32,
            line: u32,
            column: u32,
        );
        #[cxx_name = "end_stack"]
        fn end_stack_ffi(self: &mut Tracker) -> u32;

        fn close(self: &mut Tracker);
    }

    // C++ listeners (`workerd::AsyncTraceListener`). The shims catch and log listener exceptions.
    unsafe extern "C++" {
        include!("workerd/rust/async-trace/listener.h");

        #[namespace = "workerd"]
        type AsyncTraceListener;

        fn listener_context_begin(listener: Pin<&mut AsyncTraceListener>, ctx: u64, isolate: u64);
        #[expect(clippy::too_many_arguments, reason = "mirrors InitEvent's fields")]
        fn listener_init(
            listener: Pin<&mut AsyncTraceListener>,
            ctx: u64,
            id: u64,
            trigger: u64,
            execution: u64,
            kind: u8,
            name: &str,
            at: u64,
            stack: u32,
        );
        fn listener_settle(
            listener: Pin<&mut AsyncTraceListener>,
            ctx: u64,
            id: u64,
            outcome: u8,
            at: u64,
        );
        fn listener_before(listener: Pin<&mut AsyncTraceListener>, ctx: u64, id: u64, at: u64);
        fn listener_after(listener: Pin<&mut AsyncTraceListener>, ctx: u64, id: u64, at: u64);
        fn listener_destroy(listener: Pin<&mut AsyncTraceListener>, ctx: u64, id: u64, at: u64);
        fn listener_annotate(
            listener: Pin<&mut AsyncTraceListener>,
            ctx: u64,
            id: u64,
            key: &str,
            value: &str,
        );
        fn listener_turn(
            listener: Pin<&mut AsyncTraceListener>,
            ctx: u64,
            cause: u64,
            start: u64,
            has_locked: bool,
            locked: u64,
            end: u64,
        );
        /// A stack is passed as `listener_stack_begin`, a `listener_stack_frame` per frame
        /// (innermost first), then `listener_stack_end`, which calls the listener.
        fn listener_stack_begin(listener: Pin<&mut AsyncTraceListener>);
        fn listener_stack_frame(
            listener: Pin<&mut AsyncTraceListener>,
            function: &str,
            script: &str,
            script_id: i32,
            line: u32,
            column: u32,
        );
        fn listener_stack_end(listener: Pin<&mut AsyncTraceListener>, isolate: u64, id: u32);
        #[expect(clippy::too_many_arguments, reason = "mirrors ContextStats's fields")]
        fn listener_context_end(
            listener: Pin<&mut AsyncTraceListener>,
            ctx: u64,
            at: u64,
            created: u64,
            dropped: u64,
            unknown: u64,
            unbalanced: u64,
            ambiguous_bindings: u64,
            foreign_thread: u64,
        );
    }
}

use bridge::AsyncTraceListener;

// SAFETY: a listener is only called, and destroyed, on its tracker's thread. The C++ wrapper
// (`workerd::AsyncTracker`) drops calls from other threads and closes the tracker, which destroys
// its sinks, on the owning thread. Moving the `KjOwn` between threads is all that `Send` permits
// here.
unsafe impl Send for AsyncTraceListener {}

/// A sink forwarding to a C++ `AsyncTraceListener`.
struct CppSink {
    listener: KjOwn<AsyncTraceListener>,
}

impl CppSink {
    fn listener(&mut self) -> Pin<&mut AsyncTraceListener> {
        self.listener.as_mut()
    }
}

impl Sink for CppSink {
    fn context_begin(&mut self, ctx: ContextId, info: &ContextInfo<'_>) {
        bridge::listener_context_begin(self.listener(), ctx, info.isolate);
    }

    fn init(&mut self, ctx: ContextId, event: &InitEvent<'_>) {
        bridge::listener_init(
            self.listener(),
            ctx,
            event.id,
            event.trigger,
            event.execution,
            event.kind as u8,
            event.name,
            event.at,
            event.stack.unwrap_or(0),
        );
    }

    fn settle(&mut self, ctx: ContextId, id: AsyncId, outcome: Outcome, at: Nanos) {
        bridge::listener_settle(self.listener(), ctx, id, outcome as u8, at);
    }

    fn before(&mut self, ctx: ContextId, id: AsyncId, at: Nanos) {
        bridge::listener_before(self.listener(), ctx, id, at);
    }

    fn after(&mut self, ctx: ContextId, id: AsyncId, at: Nanos) {
        bridge::listener_after(self.listener(), ctx, id, at);
    }

    fn destroy(&mut self, ctx: ContextId, id: AsyncId, at: Nanos) {
        bridge::listener_destroy(self.listener(), ctx, id, at);
    }

    fn annotate(&mut self, ctx: ContextId, id: AsyncId, key: &str, value: &str) {
        bridge::listener_annotate(self.listener(), ctx, id, key, value);
    }

    fn turn(&mut self, ctx: ContextId, turn: &Turn) {
        bridge::listener_turn(
            self.listener(),
            ctx,
            turn.cause,
            turn.start,
            turn.locked.is_some(),
            turn.locked.unwrap_or(0),
            turn.end,
        );
    }

    fn stack(&mut self, isolate: IsolateId, id: StackId, frames: &[Frame]) {
        bridge::listener_stack_begin(self.listener());
        for frame in frames {
            bridge::listener_stack_frame(
                self.listener(),
                &frame.function,
                &frame.script,
                frame.script_id,
                frame.line,
                frame.column,
            );
        }
        bridge::listener_stack_end(self.listener(), isolate, id);
    }

    fn context_end(&mut self, ctx: ContextId, at: Nanos, stats: &ContextStats) {
        bridge::listener_context_end(
            self.listener(),
            ctx,
            at,
            stats.created,
            stats.dropped,
            stats.unknown,
            stats.unbalanced,
            stats.ambiguous_bindings,
            stats.foreign_thread,
        );
    }
}

pub struct Isolate(Arc<IsolateState>);

pub struct Writer(Arc<NdjsonWriter>);

#[expect(
    clippy::unnecessary_box_returns,
    reason = "c++ expects heap-allocation"
)]
fn new_isolate() -> Box<Isolate> {
    Box::new(Isolate(Arc::new(IsolateState::new())))
}

fn lossy(bytes: &[u8]) -> Cow<'_, str> {
    String::from_utf8_lossy(bytes)
}

fn new_tracker(isolate: &Isolate, worker: &[u8], actor: &[u8]) -> Box<Tracker> {
    let actor = lossy(actor);
    let actor = if actor.is_empty() {
        None
    } else {
        Some(&*actor)
    };
    Box::new(Tracker::new(
        Arc::clone(&isolate.0),
        &lossy(worker),
        actor,
        Box::new(MonotonicClock),
    ))
}

fn open_ndjson_writer(path: &[u8], producer_version: &[u8]) -> io::Result<Box<Writer>> {
    let path = std::str::from_utf8(path).map_err(io::Error::other)?;
    let writer = NdjsonWriter::create(path, "workerd", &lossy(producer_version))
        .map_err(|error| io::Error::new(error.kind(), format!("{path}: {error}")))?;
    Ok(Box::new(Writer(Arc::new(writer))))
}

impl Writer {
    fn failed(&self) -> bool {
        self.0.failed()
    }

    fn flush(&self) {
        self.0.flush();
    }
}

impl Tracker {
    fn add_ndjson_sink(&mut self, writer: &Writer) {
        self.add_sink(Box::new(NdjsonSink::new(Arc::clone(&writer.0))));
    }

    fn add_cpp_sink(&mut self, listener: KjOwn<AsyncTraceListener>) {
        self.add_sink(Box::new(CppSink { listener }));
    }

    fn create_ffi(&mut self, kind: u8, name: &[u8], trigger: u64, stack: u32) -> u64 {
        let stack = if stack == 0 { None } else { Some(stack) };
        self.create(Kind::from_u8(kind), &lossy(name), trigger, stack)
    }

    fn annotate_ffi(&mut self, id: u64, key: &[u8], value: &[u8]) {
        self.annotate(id, &lossy(key), &lossy(value));
    }

    fn push_frame_ffi(
        &mut self,
        function: &[u8],
        script: &[u8],
        script_id: i32,
        line: u32,
        column: u32,
    ) {
        self.push_frame(&lossy(function), &lossy(script), script_id, line, column);
    }

    fn settle_ffi(&mut self, id: u64, outcome: u8) {
        self.settle(id, Outcome::from_u8(outcome));
    }

    fn end_stack_ffi(&mut self) -> u32 {
        self.end_stack().unwrap_or(0)
    }
}

#[cfg(test)]
#[path = "ffi-test.rs"]
mod tests;
