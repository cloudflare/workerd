// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The per-`IoContext` tracker.

use std::collections::HashMap;
use std::collections::HashSet;
use std::sync::Arc;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;

use crate::AsyncId;
use crate::Clock;
use crate::ContextId;
use crate::ContextInfo;
use crate::ContextStats;
use crate::Frame;
use crate::InitEvent;
use crate::IsolateState;
use crate::Kind;
use crate::Nanos;
use crate::Outcome;
use crate::Sink;
use crate::StackId;

static NEXT_CONTEXT: AtomicU64 = AtomicU64::new(1);

/// Default cap on resources a tracker keeps state for at once.
pub const DEFAULT_MAX_LIVE: usize = 100_000;

/// A completed turn (one entry into JavaScript from the event loop).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Turn {
    /// The resource whose completion started the turn, or `0` if unknown.
    pub cause: AsyncId,
    /// When the turn was requested, before any lock was acquired.
    pub start: Nanos,
    /// When the locks were held and JavaScript could start, if reported.
    pub locked: Option<Nanos>,
    pub end: Nanos,
}

/// What the tracker keeps for a live resource.
struct Resource {
    settled: bool,
    /// Handles still held. Creation makes one; adoption by a bridge adds one. The resource is
    /// forgotten when the last is released.
    holders: u32,
}

/// A callback scope on the execution stack.
struct Scope {
    id: AsyncId,
    /// Whether `before` was reported (it isn't for unknown IDs), so `after` should be too.
    reported: bool,
}

struct OpenTurn {
    seq: u64,
    start: Nanos,
    locked: Option<Nanos>,
    cause: AsyncId,
    /// Whether `cause` was set by `set_turn_cause`, rather than being the default from
    /// `turn_begin`.
    explicit_cause: bool,
    /// The default cause's scope has not been entered yet. It is entered (reporting `before`)
    /// only when something in the turn is attributed to it; see [`Tracker::turn_begin`].
    default_pending: bool,
    /// Length of the scope stack when the turn began. Scopes above it belong to this turn.
    base: usize,
}

/// Owned copy of the context description, replayed to each sink as it is added.
struct Info {
    worker: String,
    actor: Option<String>,
    at: Nanos,
}

/// Tracks the async resources of one `IoContext` and reports their events to sinks.
///
/// Operations on a closed tracker, and on ID `0` (a resource that was never recorded), do
/// nothing. Mistakes by the caller (unknown IDs, unbalanced scopes, ambiguous bindings) never
/// panic; they are counted in [`ContextStats`] and reported when the tracker closes.
pub struct Tracker {
    ctx: ContextId,
    isolate: Arc<IsolateState>,
    clock: Box<dyn Clock + Send>,
    info: Info,
    sinks: Vec<Box<dyn Sink + Send>>,
    resources: HashMap<AsyncId, Resource>,
    scopes: Vec<Scope>,
    turns: Vec<OpenTurn>,
    next_turn_seq: u64,
    /// Operations that a bridge may still adopt, with the turn that created them. An entry is
    /// removed when the operation is adopted, explicitly bound, settled or destroyed, or when its
    /// turn ends.
    pending_ops: Vec<(AsyncId, u64)>,
    /// Stacks already reported to this tracker's sinks.
    reported_stacks: HashSet<StackId>,
    /// Frames collected between `begin_stack` and `end_stack`.
    building_stack: Vec<Frame>,
    stats: ContextStats,
    max_live: usize,
    closed: bool,
}

impl Tracker {
    #[must_use]
    pub fn new(
        isolate: Arc<IsolateState>,
        worker: &str,
        actor: Option<&str>,
        clock: Box<dyn Clock + Send>,
    ) -> Self {
        let at = clock.now();
        Self {
            ctx: NEXT_CONTEXT.fetch_add(1, Ordering::Relaxed),
            isolate,
            clock,
            info: Info {
                worker: worker.to_owned(),
                actor: actor.map(str::to_owned),
                at,
            },
            sinks: Vec::new(),
            resources: HashMap::new(),
            scopes: Vec::new(),
            turns: Vec::new(),
            next_turn_seq: 1,
            pending_ops: Vec::new(),
            reported_stacks: HashSet::new(),
            building_stack: Vec::new(),
            stats: ContextStats::default(),
            max_live: DEFAULT_MAX_LIVE,
            closed: false,
        }
    }

    #[must_use]
    pub const fn context_id(&self) -> ContextId {
        self.ctx
    }

    #[must_use]
    pub const fn stats(&self) -> &ContextStats {
        &self.stats
    }

    /// Caps the number of live resources. Creations beyond it return `0` and count as dropped.
    pub const fn set_max_live(&mut self, max_live: usize) {
        self.max_live = max_live;
    }

    /// Adds a sink. It is told about the context first, so it sees a complete stream.
    pub fn add_sink(&mut self, mut sink: Box<dyn Sink + Send>) {
        if self.closed {
            return;
        }
        let info = ContextInfo {
            isolate: self.isolate.id(),
            worker: &self.info.worker,
            actor: self.info.actor.as_deref(),
            at: self.info.at,
        };
        sink.context_begin(self.ctx, &info);
        self.sinks.push(sink);
    }

    /// The resource whose callback is running (`executionAsyncId`), or `0`.
    #[must_use]
    pub fn current(&self) -> AsyncId {
        if let Some(turn) = self.turns.last()
            && turn.default_pending
        {
            // Nothing has been entered in this turn yet.
            return turn.cause;
        }
        self.scopes.last().map_or(0, |scope| scope.id)
    }

    /// The cause of the innermost open turn, or `0`.
    #[must_use]
    pub fn turn_cause(&self) -> AsyncId {
        self.turns.last().map_or(0, |turn| turn.cause)
    }

    /// Records a new resource and returns its ID, or `0` if it was not recorded (tracker closed,
    /// or the live-resource cap reached). A `trigger` of `0` means the current turn's cause.
    pub fn create(
        &mut self,
        kind: Kind,
        name: &str,
        trigger: AsyncId,
        stack: Option<StackId>,
    ) -> AsyncId {
        if self.closed {
            return 0;
        }
        if self.resources.len() >= self.max_live {
            self.stats.dropped += 1;
            return 0;
        }
        let id = self.isolate.next_async_id();
        let trigger = if trigger == 0 {
            self.turn_cause()
        } else {
            trigger
        };
        self.enter_pending_default();
        let execution = self.current();
        self.resources.insert(
            id,
            Resource {
                settled: false,
                holders: 1,
            },
        );
        self.stats.created += 1;
        if kind == Kind::Operation
            && let Some(turn) = self.turns.last()
        {
            self.pending_ops.push((id, turn.seq));
        }
        if let Some(stack) = stack {
            self.report_stack(stack);
        }
        let event = InitEvent {
            id,
            trigger,
            execution,
            kind,
            name,
            at: self.clock.now(),
            stack,
        };
        for sink in &mut self.sinks {
            sink.init(self.ctx, &event);
        }
        id
    }

    /// The work behind `id` finished. Only the first settle is reported.
    pub fn settle(&mut self, id: AsyncId, outcome: Outcome) {
        if self.closed || id == 0 {
            return;
        }
        let Some(resource) = self.resources.get_mut(&id) else {
            self.stats.unknown += 1;
            return;
        };
        if resource.settled {
            return;
        }
        resource.settled = true;
        self.unpend(id);
        let at = self.clock.now();
        for sink in &mut self.sinks {
            sink.settle(self.ctx, id, outcome, at);
        }
    }

    /// The caller no longer holds `id`, because it was canceled or its handle was dropped. When
    /// the last handle is released, the tracker forgets the resource, and reports `destroy` if it
    /// never settled.
    pub fn destroy(&mut self, id: AsyncId) {
        if self.closed || id == 0 {
            return;
        }
        let Some(resource) = self.resources.get_mut(&id) else {
            self.stats.unknown += 1;
            return;
        };
        if resource.holders > 1 {
            resource.holders -= 1;
            return;
        }
        let Some(resource) = self.resources.remove(&id) else {
            return;
        };
        self.unpend(id);
        if !resource.settled {
            let at = self.clock.now();
            for sink in &mut self.sinks {
                sink.destroy(self.ctx, id, at);
            }
        }
    }

    pub fn annotate(&mut self, id: AsyncId, key: &str, value: &str) {
        if self.closed || id == 0 {
            return;
        }
        if !self.resources.contains_key(&id) {
            self.stats.unknown += 1;
            return;
        }
        for sink in &mut self.sinks {
            sink.annotate(self.ctx, id, key, value);
        }
    }

    /// A callback attributed to `id` starts. Must be matched by [`Tracker::exit`].
    pub fn enter(&mut self, id: AsyncId) {
        if self.closed || id == 0 {
            return;
        }
        self.enter_pending_default();
        // An unknown ID is still pushed, so the matching exit stays balanced.
        let reported = self.resources.contains_key(&id);
        if !reported {
            self.stats.unknown += 1;
        }
        self.scopes.push(Scope { id, reported });
        if reported {
            let at = self.clock.now();
            for sink in &mut self.sinks {
                sink.before(self.ctx, id, at);
            }
        }
    }

    /// The callback for `id` ends. If it is not the innermost scope, the scopes inside it are
    /// discarded (and counted as unbalanced). An `exit` never closes a scope from an enclosing
    /// turn.
    pub fn exit(&mut self, id: AsyncId) {
        if self.closed || id == 0 {
            return;
        }
        let base = self.turns.last().map_or(0, |turn| turn.base);
        let Some(offset) = self.scopes[base..].iter().rposition(|scope| scope.id == id) else {
            self.stats.unbalanced += 1;
            return;
        };
        let position = base + offset;
        let discarded = self.scopes.len() - position - 1;
        if discarded > 0 {
            self.stats.unbalanced += u64::try_from(discarded).unwrap_or(u64::MAX);
        }
        let reported = self.scopes[position].reported;
        self.scopes.truncate(position);
        if reported {
            let at = self.clock.now();
            for sink in &mut self.sinks {
                sink.after(self.ctx, id, at);
            }
        }
    }

    /// A turn starts: JavaScript is about to be entered from the event loop. Turns nest.
    ///
    /// `default_cause` (`0` for none) is the cause to assume unless [`Tracker::set_turn_cause`]
    /// names a better one, typically the context's current request. Its scope is entered lazily:
    /// only when the turn creates a resource, enters a callback scope, or starts a nested turn
    /// before any explicit cause is set. So a turn whose cause is set immediately (a bridge
    /// resuming JavaScript) reports no `before`/`after` for the default.
    pub fn turn_begin(&mut self, default_cause: AsyncId) {
        if self.closed {
            return;
        }
        self.enter_pending_default();
        let seq = self.next_turn_seq;
        self.next_turn_seq += 1;
        self.turns.push(OpenTurn {
            seq,
            start: self.clock.now(),
            locked: None,
            cause: default_cause,
            explicit_cause: false,
            default_pending: default_cause != 0,
            base: self.scopes.len(),
        });
    }

    /// The current turn holds its locks; JavaScript can run.
    pub fn turn_locked(&mut self) {
        if self.closed {
            return;
        }
        let now = self.clock.now();
        if let Some(turn) = self.turns.last_mut()
            && turn.locked.is_none()
        {
            turn.locked = Some(now);
        }
    }

    /// `id` started the current turn. It becomes the turn's cause (the default trigger of
    /// everything created in the turn) and its callback scope stays open until the turn ends,
    /// so it covers the microtask drain. It replaces the default cause from
    /// [`Tracker::turn_begin`], whose scope is closed.
    pub fn set_turn_cause(&mut self, id: AsyncId) {
        if self.closed || id == 0 {
            return;
        }
        let Some(turn) = self.turns.last_mut() else {
            self.stats.unbalanced += 1;
            return;
        };
        if turn.explicit_cause {
            // One cause per turn. A second is a caller bug; keep the first.
            self.stats.unbalanced += 1;
            return;
        }
        let default_cause = turn.cause;
        let default_entered = !turn.default_pending;
        turn.cause = id;
        turn.explicit_cause = true;
        turn.default_pending = false;
        if default_entered {
            self.exit(default_cause);
        }
        self.enter(id);
    }

    /// The current turn ends. Closes the cause's scope, counts any other scope left open as
    /// unbalanced, and reports the turn.
    pub fn turn_end(&mut self) {
        if self.closed {
            return;
        }
        let Some(turn) = self.turns.pop() else {
            self.stats.unbalanced += 1;
            return;
        };
        let at = self.clock.now();
        while self.scopes.len() > turn.base {
            let Some(scope) = self.scopes.pop() else {
                break;
            };
            if scope.id == turn.cause {
                if scope.reported {
                    for sink in &mut self.sinks {
                        sink.after(self.ctx, scope.id, at);
                    }
                }
            } else {
                self.stats.unbalanced += 1;
            }
        }
        self.pending_ops.retain(|&(_, seq)| seq != turn.seq);
        let event = Turn {
            cause: turn.cause,
            start: turn.start,
            locked: turn.locked,
            end: at,
        };
        for sink in &mut self.sinks {
            sink.turn(self.ctx, &event);
        }
        if self.turns.is_empty() {
            for sink in &mut self.sinks {
                sink.flush();
            }
        }
    }

    /// Called by a KJ→JS bridge. Returns the operation the bridge should report under instead of
    /// creating its own resource, or `0` if there is none.
    ///
    /// Eligible operations were created in the current turn and are neither settled nor bound.
    /// The most recently created wins; if several were eligible the binding is counted as
    /// ambiguous.
    ///
    /// The bridge becomes a second holder of the operation: it stays known until both the bridge
    /// and the operation's creator release it (see [`Tracker::destroy`]). A binding's span
    /// typically ends before its bridge's continuation runs.
    pub fn adopt_operation(&mut self) -> AsyncId {
        if self.closed {
            return 0;
        }
        let Some(turn) = self.turns.last() else {
            return 0;
        };
        let seq = turn.seq;
        let mut candidates = self
            .pending_ops
            .iter()
            .enumerate()
            .filter(|&(_, &(_, op_seq))| op_seq == seq)
            .map(|(index, _)| index);
        let Some(mut chosen) = candidates.next() else {
            return 0;
        };
        let mut ambiguous = false;
        for index in candidates {
            ambiguous = true;
            chosen = index;
        }
        if ambiguous {
            self.stats.ambiguous_bindings += 1;
        }
        let id = self.pending_ops.remove(chosen).0;
        if let Some(resource) = self.resources.get_mut(&id) {
            resource.holders += 1;
        }
        id
    }

    /// Takes `id` out of consideration for [`Tracker::adopt_operation`]: a bridge was bound to it
    /// explicitly, or it will never be awaited through a bridge.
    pub fn mark_bound(&mut self, id: AsyncId) {
        self.unpend(id);
    }

    /// Adds `count` events that the C++ wrapper dropped because they came from a thread other
    /// than the context's.
    pub const fn count_foreign_thread(&mut self, count: u64) {
        self.stats.foreign_thread = self.stats.foreign_thread.saturating_add(count);
    }

    /// Starts collecting a creation stack, innermost frame first.
    pub fn begin_stack(&mut self) {
        self.building_stack.clear();
    }

    pub fn push_frame(
        &mut self,
        function: &str,
        script: &str,
        script_id: i32,
        line: u32,
        column: u32,
    ) {
        self.building_stack.push(Frame {
            function: function.to_owned(),
            script: script.to_owned(),
            script_id,
            line,
            column,
        });
    }

    /// Finishes the stack begun by [`Tracker::begin_stack`] and returns its ID for
    /// [`Tracker::create`], or `None` if it was empty.
    pub fn end_stack(&mut self) -> Option<StackId> {
        let frames = std::mem::take(&mut self.building_stack);
        self.isolate.intern_stack(frames)
    }

    /// Ends tracking: reports the context's stats and flushes the sinks. Later calls do nothing.
    /// Dropping the tracker closes it.
    pub fn close(&mut self) {
        if self.closed {
            return;
        }
        if !self.turns.is_empty() {
            self.stats.unbalanced += u64::try_from(self.turns.len()).unwrap_or(u64::MAX);
        }
        let at = self.clock.now();
        for sink in &mut self.sinks {
            sink.context_end(self.ctx, at, &self.stats);
            sink.flush();
        }
        self.closed = true;
        self.resources = HashMap::new();
        self.scopes = Vec::new();
        self.turns = Vec::new();
        self.pending_ops = Vec::new();
        self.sinks = Vec::new();
    }

    /// Enters the innermost turn's default cause, if it is still pending.
    fn enter_pending_default(&mut self) {
        let Some(turn) = self.turns.last_mut() else {
            return;
        };
        if !turn.default_pending {
            return;
        }
        turn.default_pending = false;
        let id = turn.cause;
        self.enter(id);
    }

    fn unpend(&mut self, id: AsyncId) {
        self.pending_ops.retain(|&(op, _)| op != id);
    }

    fn report_stack(&mut self, stack: StackId) {
        if !self.reported_stacks.insert(stack) {
            return;
        }
        if let Some(frames) = self.isolate.stack(stack) {
            let isolate = self.isolate.id();
            for sink in &mut self.sinks {
                sink.stack(isolate, stack, &frames);
            }
        }
    }
}

impl Drop for Tracker {
    fn drop(&mut self) {
        self.close();
    }
}

#[cfg(test)]
#[path = "tracker-test.rs"]
mod tests;
