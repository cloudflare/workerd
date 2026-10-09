// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Async activity tracking, in the style of Node.js `async_hooks`.
//!
//! A [`Tracker`] belongs to one `IoContext`. Instrumentation in C++ (turns in
//! `IoContext::runImpl`, `awaitIo`/`awaitJs` bridges, timers, binding operations) reports
//! resource lifecycle events to it, and it fans them out to [`Sink`]s.
//!
//! # Events
//!
//! A resource is anything that later causes work: a timer, a KJ→JS bridge, a binding operation, a
//! request. Its lifecycle is `init`, then any number of `before`/`after` pairs (callbacks
//! attributed to it), with `settle` when the underlying work finishes and `destroy` if it is
//! dropped unfinished.
//!
//! # Turns
//!
//! A turn is one entry into JavaScript from the event loop: lock, run, drain microtasks, unlock.
//! The microtask queue is empty when a turn starts, so everything that runs in a turn is caused by
//! the event that started it, the turn's *cause*. Resources created during a turn get the cause
//! as their default trigger, which gives causal edges without V8 promise hooks.
//!
//! # Threads
//!
//! A [`Tracker`] is used by one thread at a time (the `IoContext`'s); the C++ wrapper enforces
//! that and is the only caller, so `Tracker` does no locking. [`IsolateState`] is shared by the
//! trackers of one isolate, which may move between threads, so it is `Send + Sync`.

mod clock;
mod ffi;
mod isolate;
mod ndjson;
mod recording;
mod tracker;

pub use clock::Clock;
pub use clock::MonotonicClock;
pub use clock::Nanos;
pub use clock::epoch_unix_ms;
pub use isolate::Frame;
pub use isolate::IsolateId;
pub use isolate::IsolateState;
pub use isolate::StackId;
pub use ndjson::FORMAT_VERSION;
pub use ndjson::NdjsonSink;
pub use ndjson::NdjsonWriter;
pub use recording::Event;
pub use recording::RecordingSink;
pub use tracker::Tracker;
pub use tracker::Turn;

/// Identifies a resource. Allocated per isolate; `0` means "none".
pub type AsyncId = u64;

/// Identifies a tracker (one per `IoContext`) within the process.
pub type ContextId = u64;

/// What kind of mechanism a resource is. Product-level detail (`kv_get`, `setTimeout`) goes in
/// the resource's name.
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Kind {
    /// The root of an incoming request (one per `IncomingRequest::delivered()`).
    Request,
    /// An `awaitIo` bridge that did not adopt a binding operation.
    KjToJs,
    /// An `awaitJs` bridge.
    JsToKj,
    /// `setTimeout`, `setInterval`, `setImmediate`, `scheduler.wait`.
    Timer,
    /// `queueMicrotask`.
    Microtask,
    /// A binding operation, named after its trace span.
    Operation,
    /// A JavaScript promise (promise-hook tier only).
    JsPromise,
    /// Anything else, including turns with no better-known cause.
    Other,
}

impl Kind {
    /// Converts the FFI representation. Unknown values map to [`Kind::Other`].
    #[must_use]
    pub const fn from_u8(value: u8) -> Self {
        match value {
            0 => Self::Request,
            1 => Self::KjToJs,
            2 => Self::JsToKj,
            3 => Self::Timer,
            4 => Self::Microtask,
            5 => Self::Operation,
            6 => Self::JsPromise,
            _ => Self::Other,
        }
    }
}

/// How the work behind a resource finished.
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Outcome {
    Ok,
    Error,
    Canceled,
}

impl Outcome {
    /// Converts the FFI representation. Unknown values map to [`Outcome::Error`].
    #[must_use]
    pub const fn from_u8(value: u8) -> Self {
        match value {
            0 => Self::Ok,
            2 => Self::Canceled,
            _ => Self::Error,
        }
    }
}

/// A resource was created.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct InitEvent<'a> {
    pub id: AsyncId,
    /// The resource whose completion caused this one to be created (default: the turn's cause).
    pub trigger: AsyncId,
    /// The resource whose callback was running when this one was created.
    pub execution: AsyncId,
    pub kind: Kind,
    pub name: &'a str,
    pub at: Nanos,
    pub stack: Option<StackId>,
}

/// Describes the `IoContext` a tracker belongs to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ContextInfo<'a> {
    pub isolate: IsolateId,
    pub worker: &'a str,
    pub actor: Option<&'a str>,
    pub at: Nanos,
}

/// Counters reported when a tracker closes. Each counts events the tracker could not attribute
/// correctly; all zero means the trace is complete.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ContextStats {
    /// Resources created.
    pub created: u64,
    /// Resources not recorded because the live-resource cap was reached.
    pub dropped: u64,
    /// Events naming an ID this tracker does not know.
    pub unknown: u64,
    /// `exit`s that did not match the innermost `enter`, and scopes left open at turn end.
    pub unbalanced: u64,
    /// Bridges that adopted an operation while more than one was eligible.
    pub ambiguous_bindings: u64,
    /// Names for a bridge's operation (C++ `IoContext::AwaitIoOperation`) that no bridge took.
    pub unused_operation_names: u64,
    /// Events dropped because they arrived on a thread other than the context's.
    pub foreign_thread: u64,
}

/// The resource of another context that caused a resource (see [`Tracker::link`]). IDs are per
/// isolate, so the isolate is part of the reference.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Link {
    pub isolate: IsolateId,
    pub ctx: ContextId,
    pub id: AsyncId,
}

/// Receives a tracker's events. Every method but [`Sink::init`] defaults to doing nothing, so a
/// sink implements only what it consumes.
#[expect(
    unused_variables,
    reason = "default methods ignore their arguments; the names document the trait"
)]
pub trait Sink {
    fn context_begin(&mut self, ctx: ContextId, info: &ContextInfo<'_>) {}
    fn init(&mut self, ctx: ContextId, event: &InitEvent<'_>);
    fn settle(&mut self, ctx: ContextId, id: AsyncId, outcome: Outcome, at: Nanos) {}
    fn before(&mut self, ctx: ContextId, id: AsyncId, at: Nanos) {}
    fn after(&mut self, ctx: ContextId, id: AsyncId, at: Nanos) {}
    fn destroy(&mut self, ctx: ContextId, id: AsyncId, at: Nanos) {}
    fn annotate(&mut self, ctx: ContextId, id: AsyncId, key: &str, value: &str) {}
    fn turn(&mut self, ctx: ContextId, turn: &Turn) {}
    /// Resource `id` was caused by `from`, a resource of another context.
    fn link(&mut self, ctx: ContextId, id: AsyncId, from: &Link) {}
    /// Called the first time this tracker reports a resource with stack `id`.
    fn stack(&mut self, isolate: IsolateId, id: StackId, frames: &[Frame]) {}
    fn context_end(&mut self, ctx: ContextId, at: Nanos, stats: &ContextStats) {}
    /// Called at the end of every outermost turn and when the tracker closes. Buffering sinks
    /// write out here.
    fn flush(&mut self) {}
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
