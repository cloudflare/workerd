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

use std::io;
use std::sync::Arc;

use crate::IsolateState;
use crate::Kind;
use crate::MonotonicClock;
use crate::NdjsonSink;
use crate::NdjsonWriter;
use crate::Outcome;
use crate::Tracker;

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
        fn new_tracker(isolate: &Isolate, worker: &str, actor: &str) -> Box<Tracker>;

        /// Creates (or truncates) `path` and writes the header.
        fn open_ndjson_writer(path: &str, producer_version: &str) -> Result<Box<Writer>>;
        fn failed(self: &Writer) -> bool;

        fn add_ndjson_sink(self: &mut Tracker, writer: &Writer);

        #[cxx_name = "create"]
        fn create_ffi(self: &mut Tracker, kind: u8, name: &str, trigger: u64, stack: u32) -> u64;
        #[cxx_name = "settle"]
        fn settle_ffi(self: &mut Tracker, id: u64, outcome: u8);
        fn destroy(self: &mut Tracker, id: u64);
        fn annotate(self: &mut Tracker, id: u64, key: &str, value: &str);
        fn enter(self: &mut Tracker, id: u64);
        fn exit(self: &mut Tracker, id: u64);
        fn current(self: &Tracker) -> u64;

        fn turn_begin(self: &mut Tracker);
        fn turn_locked(self: &mut Tracker);
        fn set_turn_cause(self: &mut Tracker, id: u64);
        fn turn_end(self: &mut Tracker);

        fn adopt_operation(self: &mut Tracker) -> u64;
        fn mark_bound(self: &mut Tracker, id: u64);

        fn begin_stack(self: &mut Tracker);
        fn push_frame(
            self: &mut Tracker,
            function: &str,
            script: &str,
            script_id: i32,
            line: u32,
            column: u32,
        );
        #[cxx_name = "end_stack"]
        fn end_stack_ffi(self: &mut Tracker) -> u32;

        fn close(self: &mut Tracker);
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

fn new_tracker(isolate: &Isolate, worker: &str, actor: &str) -> Box<Tracker> {
    let actor = if actor.is_empty() { None } else { Some(actor) };
    Box::new(Tracker::new(
        Arc::clone(&isolate.0),
        worker,
        actor,
        Box::new(MonotonicClock),
    ))
}

fn open_ndjson_writer(path: &str, producer_version: &str) -> io::Result<Box<Writer>> {
    let writer = NdjsonWriter::create(path, "workerd", producer_version)?;
    Ok(Box::new(Writer(Arc::new(writer))))
}

impl Writer {
    fn failed(&self) -> bool {
        self.0.failed()
    }
}

impl Tracker {
    fn add_ndjson_sink(&mut self, writer: &Writer) {
        self.add_sink(Box::new(NdjsonSink::new(Arc::clone(&writer.0))));
    }

    fn create_ffi(&mut self, kind: u8, name: &str, trigger: u64, stack: u32) -> u64 {
        let stack = if stack == 0 { None } else { Some(stack) };
        self.create(Kind::from_u8(kind), name, trigger, stack)
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
