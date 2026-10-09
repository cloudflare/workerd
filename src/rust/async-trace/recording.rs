// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A sink that keeps events in memory, for tests.

use std::sync::Arc;
use std::sync::Mutex;
use std::sync::PoisonError;

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

/// An event as recorded by [`RecordingSink`]. Timestamps are dropped, except in turns, so tests
/// can compare sequences directly.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    ContextBegin {
        ctx: ContextId,
        isolate: IsolateId,
        worker: String,
        actor: Option<String>,
    },
    Init {
        ctx: ContextId,
        id: AsyncId,
        trigger: AsyncId,
        execution: AsyncId,
        kind: Kind,
        name: String,
        stack: Option<StackId>,
        parent: AsyncId,
    },
    Settle {
        ctx: ContextId,
        id: AsyncId,
        outcome: Outcome,
    },
    Before {
        ctx: ContextId,
        id: AsyncId,
    },
    After {
        ctx: ContextId,
        id: AsyncId,
    },
    Destroy {
        ctx: ContextId,
        id: AsyncId,
    },
    Annotate {
        ctx: ContextId,
        id: AsyncId,
        key: String,
        value: String,
    },
    Turn {
        ctx: ContextId,
        turn: Turn,
    },
    Link {
        ctx: ContextId,
        id: AsyncId,
        from: Link,
    },
    Stack {
        isolate: IsolateId,
        id: StackId,
        frames: Vec<Frame>,
    },
    ContextEnd {
        ctx: ContextId,
        stats: ContextStats,
    },
    Flush,
}

/// Records every event into a shared list, which stays readable after the sink has been handed to
/// a tracker.
#[derive(Debug, Clone, Default)]
pub struct RecordingSink {
    events: Arc<Mutex<Vec<Event>>>,
}

impl RecordingSink {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// A copy of the events recorded so far, by this sink and its clones.
    #[must_use]
    pub fn events(&self) -> Vec<Event> {
        self.lock().clone()
    }

    /// Removes and returns the events recorded so far.
    #[must_use]
    pub fn take(&self) -> Vec<Event> {
        std::mem::take(&mut *self.lock())
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Vec<Event>> {
        self.events.lock().unwrap_or_else(PoisonError::into_inner)
    }

    fn push(&self, event: Event) {
        self.lock().push(event);
    }
}

impl Sink for RecordingSink {
    fn context_begin(&mut self, ctx: ContextId, info: &ContextInfo<'_>) {
        self.push(Event::ContextBegin {
            ctx,
            isolate: info.isolate,
            worker: info.worker.to_owned(),
            actor: info.actor.map(str::to_owned),
        });
    }

    fn init(&mut self, ctx: ContextId, event: &InitEvent<'_>) {
        self.push(Event::Init {
            ctx,
            id: event.id,
            trigger: event.trigger,
            execution: event.execution,
            kind: event.kind,
            name: event.name.to_owned(),
            stack: event.stack,
            parent: event.parent,
        });
    }

    fn settle(&mut self, ctx: ContextId, id: AsyncId, outcome: Outcome, _at: Nanos) {
        self.push(Event::Settle { ctx, id, outcome });
    }

    fn before(&mut self, ctx: ContextId, id: AsyncId, _at: Nanos) {
        self.push(Event::Before { ctx, id });
    }

    fn after(&mut self, ctx: ContextId, id: AsyncId, _at: Nanos) {
        self.push(Event::After { ctx, id });
    }

    fn destroy(&mut self, ctx: ContextId, id: AsyncId, _at: Nanos) {
        self.push(Event::Destroy { ctx, id });
    }

    fn link(&mut self, ctx: ContextId, id: AsyncId, from: &Link) {
        self.push(Event::Link {
            ctx,
            id,
            from: *from,
        });
    }

    fn annotate(&mut self, ctx: ContextId, id: AsyncId, key: &str, value: &str) {
        self.push(Event::Annotate {
            ctx,
            id,
            key: key.to_owned(),
            value: value.to_owned(),
        });
    }

    fn turn(&mut self, ctx: ContextId, turn: &Turn) {
        self.push(Event::Turn { ctx, turn: *turn });
    }

    fn stack(&mut self, isolate: IsolateId, id: StackId, frames: &[Frame]) {
        self.push(Event::Stack {
            isolate,
            id,
            frames: frames.to_vec(),
        });
    }

    fn context_end(&mut self, ctx: ContextId, _at: Nanos, stats: &ContextStats) {
        self.push(Event::ContextEnd { ctx, stats: *stats });
    }

    fn flush(&mut self) {
        self.push(Event::Flush);
    }
}
