// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The NDJSON event log: one JSON object per line, field `e` naming the event.
//!
//! The first line is a header whose `v` is [`FORMAT_VERSION`], bumped only for breaking changes.
//! Consumers must ignore event types and fields they don't know. Times are integer nanoseconds
//! since the process trace epoch; the header's `epochUnixMs` gives its wall-clock time.
//!
//! Many trackers share one [`NdjsonWriter`]. Each [`NdjsonSink`] buffers its own lines and hands
//! them to the writer whole, so lines from different contexts never interleave mid-line.

use std::fs::File;
use std::io;
use std::io::BufWriter;
use std::io::Write;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::PoisonError;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;

use serde::Serialize;

use crate::AsyncId;
use crate::ContextId;
use crate::ContextInfo;
use crate::ContextStats;
use crate::Frame;
use crate::InitEvent;
use crate::IsolateId;
use crate::Kind;
use crate::Link;
use crate::Nanos;
use crate::Outcome;
use crate::Sink;
use crate::StackId;
use crate::Turn;
use crate::epoch_unix_ms;

/// The format's major version, written in the header as `v`.
pub const FORMAT_VERSION: u32 = 1;

/// A sink buffers at most this much before writing out early.
const SINK_BUFFER_LIMIT: usize = 64 * 1024;

#[derive(Serialize)]
#[serde(tag = "e", rename_all = "snake_case", rename_all_fields = "camelCase")]
enum Line<'a> {
    Header {
        v: u32,
        producer: &'a str,
        version: &'a str,
        epoch_unix_ms: u64,
        pid: u32,
    },
    Ctx {
        ctx: ContextId,
        iso: IsolateId,
        worker: &'a str,
        actor: Option<&'a str>,
        at: Nanos,
    },
    Init {
        ctx: ContextId,
        id: AsyncId,
        kind: Kind,
        name: &'a str,
        trigger: AsyncId,
        exec: AsyncId,
        at: Nanos,
        #[serde(skip_serializing_if = "Option::is_none")]
        stack: Option<StackId>,
    },
    Settle {
        ctx: ContextId,
        id: AsyncId,
        outcome: Outcome,
        at: Nanos,
    },
    Before {
        ctx: ContextId,
        id: AsyncId,
        at: Nanos,
    },
    After {
        ctx: ContextId,
        id: AsyncId,
        at: Nanos,
    },
    Destroy {
        ctx: ContextId,
        id: AsyncId,
        at: Nanos,
    },
    Annotate {
        ctx: ContextId,
        id: AsyncId,
        k: &'a str,
        v: &'a str,
    },
    Turn {
        ctx: ContextId,
        cause: AsyncId,
        start: Nanos,
        #[serde(skip_serializing_if = "Option::is_none")]
        locked: Option<Nanos>,
        end: Nanos,
    },
    Link {
        ctx: ContextId,
        id: AsyncId,
        from_iso: IsolateId,
        from_ctx: ContextId,
        from_id: AsyncId,
    },
    Stack {
        iso: IsolateId,
        id: StackId,
        frames: &'a [Frame],
    },
    CtxEnd {
        ctx: ContextId,
        at: Nanos,
        created: u64,
        dropped: u64,
        unknown: u64,
        unbalanced: u64,
        ambiguous_bindings: u64,
        unused_operation_names: u64,
        foreign_thread: u64,
    },
}

fn write_line(buf: &mut Vec<u8>, line: &Line<'_>) -> bool {
    let start = buf.len();
    if serde_json::to_writer(&mut *buf, line).is_ok() {
        buf.push(b'\n');
        true
    } else {
        // Cannot happen for these types (no maps, no fallible Serialize impls); drop the partial
        // line rather than corrupt the stream.
        buf.truncate(start);
        false
    }
}

/// A process-wide NDJSON output. The first I/O error disables it; later writes are dropped and
/// [`NdjsonWriter::failed`] reports it.
pub struct NdjsonWriter {
    out: Mutex<Option<Box<dyn Write + Send>>>,
    failed: AtomicBool,
}

impl NdjsonWriter {
    /// Writes the header to `out` and returns a writer appending to it.
    ///
    /// # Errors
    ///
    /// Returns the error from writing the header.
    pub fn new(mut out: Box<dyn Write + Send>, producer: &str, version: &str) -> io::Result<Self> {
        let mut header = Vec::new();
        write_line(
            &mut header,
            &Line::Header {
                v: FORMAT_VERSION,
                producer,
                version,
                epoch_unix_ms: epoch_unix_ms(),
                pid: std::process::id(),
            },
        );
        out.write_all(&header)?;
        Ok(Self {
            out: Mutex::new(Some(out)),
            failed: AtomicBool::new(false),
        })
    }

    /// Creates (or truncates) the file at `path` and writes the header.
    ///
    /// # Errors
    ///
    /// Returns the error from creating the file or writing the header.
    pub fn create(path: &str, producer: &str, version: &str) -> io::Result<Self> {
        let file = File::create(path)?;
        Self::new(Box::new(BufWriter::new(file)), producer, version)
    }

    /// Whether an I/O error has disabled the writer.
    #[must_use]
    pub fn failed(&self) -> bool {
        self.failed.load(Ordering::Relaxed)
    }

    /// Appends whole lines.
    pub fn append(&self, lines: &[u8]) {
        self.with_out(|out| out.write_all(lines));
    }

    /// Flushes buffered output to the underlying file.
    pub fn flush(&self) {
        self.with_out(Write::flush);
    }

    fn with_out(&self, f: impl FnOnce(&mut Box<dyn Write + Send>) -> io::Result<()>) {
        let mut guard = self.out.lock().unwrap_or_else(PoisonError::into_inner);
        let failed = guard.as_mut().is_some_and(|out| f(out).is_err());
        if failed {
            *guard = None;
        }
        drop(guard);
        if failed {
            self.failed.store(true, Ordering::Relaxed);
        }
    }
}

impl Drop for NdjsonWriter {
    fn drop(&mut self) {
        self.flush();
    }
}

/// Serializes one tracker's events to an [`NdjsonWriter`].
pub struct NdjsonSink {
    writer: Arc<NdjsonWriter>,
    buf: Vec<u8>,
}

impl NdjsonSink {
    #[must_use]
    pub const fn new(writer: Arc<NdjsonWriter>) -> Self {
        Self {
            writer,
            buf: Vec::new(),
        }
    }

    fn push(&mut self, line: &Line<'_>) {
        write_line(&mut self.buf, line);
        if self.buf.len() >= SINK_BUFFER_LIMIT {
            self.write_out();
        }
    }

    fn write_out(&mut self) {
        if !self.buf.is_empty() {
            self.writer.append(&self.buf);
            self.buf.clear();
        }
    }
}

impl Sink for NdjsonSink {
    fn context_begin(&mut self, ctx: ContextId, info: &ContextInfo<'_>) {
        self.push(&Line::Ctx {
            ctx,
            iso: info.isolate,
            worker: info.worker,
            actor: info.actor,
            at: info.at,
        });
    }

    fn init(&mut self, ctx: ContextId, event: &InitEvent<'_>) {
        self.push(&Line::Init {
            ctx,
            id: event.id,
            kind: event.kind,
            name: event.name,
            trigger: event.trigger,
            exec: event.execution,
            at: event.at,
            stack: event.stack,
        });
    }

    fn settle(&mut self, ctx: ContextId, id: AsyncId, outcome: Outcome, at: Nanos) {
        self.push(&Line::Settle {
            ctx,
            id,
            outcome,
            at,
        });
    }

    fn before(&mut self, ctx: ContextId, id: AsyncId, at: Nanos) {
        self.push(&Line::Before { ctx, id, at });
    }

    fn after(&mut self, ctx: ContextId, id: AsyncId, at: Nanos) {
        self.push(&Line::After { ctx, id, at });
    }

    fn destroy(&mut self, ctx: ContextId, id: AsyncId, at: Nanos) {
        self.push(&Line::Destroy { ctx, id, at });
    }

    fn annotate(&mut self, ctx: ContextId, id: AsyncId, key: &str, value: &str) {
        self.push(&Line::Annotate {
            ctx,
            id,
            k: key,
            v: value,
        });
    }

    fn link(&mut self, ctx: ContextId, id: AsyncId, from: &Link) {
        self.push(&Line::Link {
            ctx,
            id,
            from_iso: from.isolate,
            from_ctx: from.ctx,
            from_id: from.id,
        });
    }

    fn turn(&mut self, ctx: ContextId, turn: &Turn) {
        self.push(&Line::Turn {
            ctx,
            cause: turn.cause,
            start: turn.start,
            locked: turn.locked,
            end: turn.end,
        });
    }

    fn stack(&mut self, isolate: IsolateId, id: StackId, frames: &[Frame]) {
        self.push(&Line::Stack {
            iso: isolate,
            id,
            frames,
        });
    }

    fn context_end(&mut self, ctx: ContextId, at: Nanos, stats: &ContextStats) {
        self.push(&Line::CtxEnd {
            ctx,
            at,
            created: stats.created,
            dropped: stats.dropped,
            unknown: stats.unknown,
            unbalanced: stats.unbalanced,
            ambiguous_bindings: stats.ambiguous_bindings,
            unused_operation_names: stats.unused_operation_names,
            foreign_thread: stats.foreign_thread,
        });
        self.write_out();
        self.writer.flush();
    }

    fn flush(&mut self) {
        self.write_out();
    }
}

impl Drop for NdjsonSink {
    fn drop(&mut self) {
        self.write_out();
    }
}

#[cfg(test)]
#[path = "ndjson-test.rs"]
mod tests;
