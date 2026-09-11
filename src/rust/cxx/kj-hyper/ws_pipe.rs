//! The in-memory WebSocket pipe: `kj::newWebSocketPipe()`'s Rust replacement under the rust
//! I/O backend (the C++ implementation lives in the unlinked kj-http-impl; the
//! //src/workerd/util:kj-http shim's `kj::newWebSocketPipe()` builds its ends over this).
//!
//! A behavior-parity port of kj's `WebSocketPipeImpl`/`WebSocketPipeEnd` (see the spec in the
//! upstreaming notes): each pipe is two independent DIRECTIONS; an end sends on one direction
//! and receives on the other (the peer end sees them swapped). A direction is a rendezvous —
//! `send()` resolves only when the peer's `receive()` (or an adopted pump) consumes the
//! message; there is NO buffering. Pumps ADOPT the foreign socket so messages flow without a
//! per-message pipe copy, with kj's exact completion semantics:
//!
//! - close is NOT terminal at the pipe level (only disconnect/abort are sticky), but a
//!   steady-state `pump_to` completes cleanly when it forwards a Close;
//! - a parked `pump_to` is FULFILLED (clean) by the peer's abort, while parked
//!   send/receive/pump_from are rejected DISCONNECTED "other end of WebSocketPipe was
//!   destroyed";
//! - `receive()`'s max_size is stored but never enforced by the rendezvous (kj parity): it is
//!   only forwarded to a real socket when a pump is adopted;
//! - byte counting matches kj's (deliberately lossy) rules: sends/closes count on the sender's
//!   direction after delivery; an adopted-source pump consumed by one-by-one `receive()` calls
//!   is NOT counted; a pump meeting an existing state credits the destination's
//!   receivedByteCount delta.
//!
//! Exception texts are kj's, byte for byte — workerd's tests assert them.

use std::cell::RefCell;
use std::future::Future;
use std::rc::Rc;

use cxx::KjError;
use cxx::KjExceptionType;
use futures::FutureExt;
use futures::select_biased;
use kj::Result;
use tokio::sync::oneshot;

use crate::ffi;
use crate::ffi::WsPtr;

const ABORTED_MSG: &str = "other end of WebSocketPipe was destroyed";
const DISCONNECTED_MSG: &str = "WebSocket disconnected";
const DISCONNECT_ENDED_PUMP_MSG: &str = "WebSocket::disconnect() ended the pump";
const DEST_ABORTED_MSG: &str = "WebSocket was aborted";
const SEND_IN_PROGRESS_MSG: &str = "another message send is already in progress";
const RECEIVE_IN_PROGRESS_MSG: &str = "another message receive is already in progress";

fn disconnected(msg: &str) -> KjError {
    KjError::new(KjExceptionType::Disconnected, msg.to_owned())
}

fn failed(msg: &str) -> KjError {
    KjError::new(KjExceptionType::Failed, msg.to_owned())
}

/// A parked (deep-copied) message. Close carries (code, reason).
#[derive(Clone)]
enum PipeMessage {
    Text(Vec<u8>),
    Binary(Vec<u8>),
    Close(u16, Vec<u8>),
}

impl PipeMessage {
    /// kj's byte accounting: payload length, plus 2 for a close's code.
    fn counted_size(&self) -> u64 {
        match self {
            Self::Text(d) | Self::Binary(d) => d.len() as u64,
            Self::Close(_, reason) => reason.len() as u64 + 2,
        }
    }

    fn into_ws_message(self) -> ffi::WsMessage {
        match self {
            Self::Text(data) => ffi::WsMessage {
                kind: ffi::WsMessageKind::TEXT,
                data,
                close_code: 0,
            },
            Self::Binary(data) => ffi::WsMessage {
                kind: ffi::WsMessageKind::BINARY,
                data,
                close_code: 0,
            },
            Self::Close(code, reason) => ffi::WsMessage {
                kind: ffi::WsMessageKind::CLOSE,
                data: reason,
                close_code: code,
            },
        }
    }

    /// Deliver this message to a foreign socket (send or close).
    async fn deliver_to(self, ws: WsPtr) -> Result<()> {
        match self {
            Self::Text(data) => ws.send_text(&data).await,
            Self::Binary(data) => ws.send_binary(&data).await,
            Self::Close(code, reason) => ws.close(code, &reason).await,
        }
    }
}

/// One direction's parked operation (kj's Blocked* states). The `busy` flags mirror kj's
/// per-state `Canceler::isEmpty()` guards: a forwarded operation is in flight through the
/// parked state, so a second same-way operation is a caller bug.
enum State {
    Idle,
    /// A send()/close() is parked awaiting a consumer. `done`: fulfilled/rejected by the
    /// consumer; dropped-without-send means the consumer never came (sender cancelled).
    BlockedSend {
        message: PipeMessage,
        done: oneshot::Sender<Result<()>>,
        busy: bool,
    },
    /// A tryPumpFrom(input) adopted `input` as this direction's message source.
    BlockedPumpFrom {
        input: WsPtr,
        done: oneshot::Sender<Result<()>>,
        busy: bool,
    },
    /// A receive() is parked awaiting a producer.
    BlockedReceive {
        max_size: usize,
        done: oneshot::Sender<Result<ffi::WsMessage>>,
        busy: bool,
    },
    /// A pumpTo(output) adopted `output` as this direction's message sink.
    BlockedPumpTo {
        output: WsPtr,
        done: oneshot::Sender<Result<()>>,
        busy: bool,
    },
    /// The sender called disconnect(); sticky.
    Disconnected,
    /// The peer end was aborted/destroyed; sticky.
    Aborted,
}

impl State {
    fn is_sticky(&self) -> bool {
        matches!(self, State::Disconnected | State::Aborted)
    }
}

/// One direction of the pipe (kj's WebSocketPipeImpl).
struct Direction {
    state: State,
    /// Bumped whenever a new parked state is installed; lets a cancelled operation's ParkGuard
    /// recognize that a later state replaced its own (kj: endState clears only if still self).
    generation: u64,
    /// Bytes delivered through this direction (kj's transferredBytes; serves both the
    /// sender's sentByteCount and the receiver's receivedByteCount).
    transferred: u64,
    /// Set once aborted; `abort_waiters` are the parked whenAborted() calls.
    aborted: bool,
    abort_waiters: Vec<oneshot::Sender<()>>,
    /// The pump destinations the OWNING end (the end using this direction as `in`) currently
    /// has active, read by the PEER end's getPreferredExtensions (kj's destinationPumpingTo/
    /// destinationPumpingFrom). Cleared when the pump promise settles or drops.
    dest_pumping_to: Option<WsPtr>,
    dest_pumping_from: Option<WsPtr>,
    /// kj's "can only call pumpTo()/tryPumpFrom() once at a time" trackers for the owning end.
    /// They live here (not on `WsPipeEnd`) because the pump future must be self-contained: kj's
    /// pump promise holds the refcounted pipe, not the end object, so the promise may be
    /// dropped after the end itself is gone (e.g. event-loop teardown) and its drop-time
    /// deregistration may only touch `Pipe`-owned state.
    pumping_to: bool,
    pumping_from: bool,
}

impl Direction {
    fn new() -> Self {
        Self {
            state: State::Idle,
            generation: 0,
            transferred: 0,
            aborted: bool::default(),
            abort_waiters: Vec::new(),
            dest_pumping_to: None,
            dest_pumping_from: None,
            pumping_to: false,
            pumping_from: false,
        }
    }

    fn abort(&mut self) {
        // Resolve the parked operation per kj's Blocked*::abort, then stick to Aborted. (When
        // already sticky, keep the existing state: aborting a Disconnected direction leaves
        // Disconnected in kj too, because Disconnected lives in ownState and abort() forwards
        // to it, which ignores.)
        let next = if matches!(self.state, State::Disconnected) {
            State::Disconnected
        } else {
            State::Aborted
        };
        let prev = std::mem::replace(&mut self.state, next);
        match prev {
            State::BlockedSend { done, .. } | State::BlockedPumpFrom { done, .. } => {
                let _ = done.send(Err(disconnected(ABORTED_MSG)));
            }
            State::BlockedReceive { done, .. } => {
                let _ = done.send(Err(disconnected(ABORTED_MSG)));
            }
            // kj: a parked pumpTo is FULFILLED (clean) by abort -- peer-drop is treated as a
            // clean disconnect for the pump.
            State::BlockedPumpTo { done, .. } => {
                let _ = done.send(Ok(()));
            }
            State::Idle | State::Disconnected | State::Aborted => {}
        }
        if !self.aborted {
            self.aborted = true;
            for waiter in self.abort_waiters.drain(..) {
                let _ = waiter.send(());
            }
        }
    }
}

/// The shared pipe: two directions.
struct Pipe {
    directions: [RefCell<Direction>; 2],
}

/// Clears a parked state on cancellation (kj: Blocked*'s destructor endState()s back to idle)
/// unless the rendezvous already replaced it. Identified by generation count so a later parked
/// state is never clobbered.
struct ParkGuard {
    pipe: Rc<Pipe>,
    dir: usize,
    generation: u64,
    armed: bool,
}

impl Drop for ParkGuard {
    fn drop(&mut self) {
        if !self.armed {
            return;
        }
        let mut dir = self.pipe.directions[self.dir].borrow_mut();
        if dir.generation == self.generation && !dir.state.is_sticky() {
            dir.state = State::Idle;
        }
    }
}

pub struct WsPipeEnd {
    pipe: Rc<Pipe>,
    /// Index of the direction this end receives from (`in`); it sends on the other one.
    input: usize,
}

impl Drop for WsPipeEnd {
    fn drop(&mut self) {
        // kj's WebSocketPipeEnd destructor: abort BOTH directions.
        self.pipe.directions[0].borrow_mut().abort();
        self.pipe.directions[1].borrow_mut().abort();
    }
}

pub fn new_websocket_pipe() -> ffi::WsPipePair {
    let pipe = Rc::new(Pipe {
        directions: [
            RefCell::new(Direction::new()),
            RefCell::new(Direction::new()),
        ],
    });
    ffi::WsPipePair {
        end1: Box::new(WsPipeEnd {
            pipe: pipe.clone(),
            input: 0,
        }),
        end2: Box::new(WsPipeEnd { pipe, input: 1 }),
    }
}

impl WsPipeEnd {
    fn output(&self) -> usize {
        1 - self.input
    }

    // --- The sender-side rendezvous (this end's OUT direction; kj's Impl::send/close). ---

    async fn send_message(&self, message: PipeMessage) -> Result<()> {
        let counted = message.counted_size();
        let dir_idx = self.output();
        // Fast path: a consumer is already parked (or the direction is sticky).
        let rx = {
            let mut dir = self.pipe.directions[dir_idx].borrow_mut();
            match &mut dir.state {
                State::Aborted => return Err(disconnected(ABORTED_MSG)),
                State::Disconnected => {
                    return Err(failed(match &message {
                        PipeMessage::Close(..) => "can't close() after disconnect()",
                        _ => "can't send() after disconnect()",
                    }));
                }
                State::BlockedReceive { busy, .. } => {
                    assert!(!*busy, "already pumping");
                    let State::BlockedReceive { done, .. } =
                        std::mem::replace(&mut dir.state, State::Idle)
                    else {
                        unreachable!()
                    };
                    let _ = done.send(Ok(message.into_ws_message()));
                    dir.transferred += counted;
                    return Ok(());
                }
                State::BlockedPumpTo { output, busy, .. } => {
                    assert!(!*busy, "{SEND_IN_PROGRESS_MSG}");
                    *busy = true;
                    let output = *output;
                    drop(dir);
                    let result = self.forward_into_pump_to(dir_idx, output, message).await;
                    if result.is_ok() {
                        self.pipe.directions[dir_idx].borrow_mut().transferred += counted;
                    }
                    return result;
                }
                State::BlockedSend { .. } | State::BlockedPumpFrom { .. } => {
                    return Err(failed(SEND_IN_PROGRESS_MSG));
                }
                State::Idle => {
                    let (tx, rx) = oneshot::channel();
                    dir.generation += 1;
                    dir.state = State::BlockedSend {
                        message,
                        done: tx,
                        busy: false,
                    };
                    rx
                }
            }
        };
        let mut guard = ParkGuard {
            pipe: self.pipe.clone(),
            dir: dir_idx,
            generation: self.pipe.directions[dir_idx].borrow().generation,
            armed: true,
        };
        let result = match rx.await {
            Ok(result) => result,
            // The consuming side dropped the fulfiller without answering: treat as abort.
            Err(_) => Err(disconnected(ABORTED_MSG)),
        };
        guard.armed = false;
        if result.is_ok() {
            self.pipe.directions[dir_idx].borrow_mut().transferred += counted;
        }
        result
    }

    /// Forward one message into an adopted pump sink (kj's BlockedPumpTo::send/close). Close
    /// completes the pump cleanly; errors reject it.
    async fn forward_into_pump_to(
        &self,
        dir_idx: usize,
        output: WsPtr,
        message: PipeMessage,
    ) -> Result<()> {
        let is_close = matches!(message, PipeMessage::Close(..));
        let result = message.deliver_to(output).await;
        let mut dir = self.pipe.directions[dir_idx].borrow_mut();
        match &mut dir.state {
            State::BlockedPumpTo { busy, .. } => {
                *busy = false;
                match (&result, is_close) {
                    (Ok(()), false) => {}
                    (Ok(()), true) => {
                        // Close terminates the pump cleanly.
                        let State::BlockedPumpTo { done, .. } =
                            std::mem::replace(&mut dir.state, State::Idle)
                        else {
                            unreachable!()
                        };
                        let _ = done.send(Ok(()));
                    }
                    (Err(e), _) => {
                        let State::BlockedPumpTo { done, .. } =
                            std::mem::replace(&mut dir.state, State::Idle)
                        else {
                            unreachable!()
                        };
                        let _ = done.send(Err(e.clone()));
                    }
                }
            }
            // The pump was cancelled/aborted while we were forwarding; nothing to update.
            _ => {}
        }
        result
    }

    pub async fn send_text(&self, text: &[u8]) -> Result<()> {
        self.send_message(PipeMessage::Text(text.to_vec())).await
    }

    pub async fn send_binary(&self, data: &[u8]) -> Result<()> {
        self.send_message(PipeMessage::Binary(data.to_vec())).await
    }

    pub async fn close(&self, code: u16, reason: &[u8]) -> Result<()> {
        self.send_message(PipeMessage::Close(code, reason.to_vec()))
            .await
    }

    pub fn disconnect(&self) {
        let mut dir = self.pipe.directions[self.output()].borrow_mut();
        match std::mem::replace(&mut dir.state, State::Disconnected) {
            State::Idle => {}
            State::BlockedReceive { done, .. } => {
                let _ = done.send(Err(disconnected(DISCONNECTED_MSG)));
            }
            State::BlockedPumpTo { output, done, .. } => {
                output.disconnect();
                let _ = done.send(Err(disconnected(DISCONNECT_ENDED_PUMP_MSG)));
            }
            sticky @ (State::Disconnected | State::Aborted) => {
                // Redundant/after-abort disconnects are ignored; restore.
                dir.state = sticky;
            }
            State::BlockedSend { .. } | State::BlockedPumpFrom { .. } => {
                panic!("{SEND_IN_PROGRESS_MSG}");
            }
        }
    }

    pub fn abort(&self) {
        self.pipe.directions[0].borrow_mut().abort();
        self.pipe.directions[1].borrow_mut().abort();
    }

    pub async fn when_aborted(&self) {
        let rx = {
            let mut dir = self.pipe.directions[self.output()].borrow_mut();
            if dir.aborted {
                return;
            }
            let (tx, rx) = oneshot::channel();
            dir.abort_waiters.push(tx);
            rx
        };
        let _ = rx.await;
    }

    // --- The receiver side (this end's IN direction; kj's Impl::receive/pumpTo). ---

    pub async fn receive(&self, max_size: usize) -> Result<ffi::WsMessage> {
        let dir_idx = self.input;
        let rx = {
            let mut dir = self.pipe.directions[dir_idx].borrow_mut();
            match &mut dir.state {
                State::Aborted => return Err(disconnected(ABORTED_MSG)),
                State::Disconnected => return Err(disconnected(DISCONNECTED_MSG)),
                State::BlockedSend { busy, .. } => {
                    assert!(!*busy, "already pumping");
                    let State::BlockedSend { message, done, .. } =
                        std::mem::replace(&mut dir.state, State::Idle)
                    else {
                        unreachable!()
                    };
                    // maxSize is deliberately NOT enforced by the rendezvous (kj parity).
                    let _ = done.send(Ok(()));
                    return Ok(message.into_ws_message());
                }
                State::BlockedPumpFrom { input, busy, .. } => {
                    assert!(!*busy, "{RECEIVE_IN_PROGRESS_MSG}");
                    *busy = true;
                    let input = *input;
                    drop(dir);
                    return self.receive_from_pump_from(dir_idx, input, max_size).await;
                }
                State::BlockedReceive { .. } | State::BlockedPumpTo { .. } => {
                    return Err(failed(RECEIVE_IN_PROGRESS_MSG));
                }
                State::Idle => {
                    let (tx, rx) = oneshot::channel();
                    dir.generation += 1;
                    dir.state = State::BlockedReceive {
                        max_size,
                        done: tx,
                        busy: false,
                    };
                    rx
                }
            }
        };
        let mut guard = ParkGuard {
            pipe: self.pipe.clone(),
            dir: dir_idx,
            generation: self.pipe.directions[dir_idx].borrow().generation,
            armed: true,
        };
        let result = match rx.await {
            Ok(result) => result,
            Err(_) => Err(disconnected(ABORTED_MSG)),
        };
        guard.armed = false;
        result
    }

    /// Pull one message from an adopted pump source (kj's BlockedPumpFrom::receive): a Close
    /// completes the pump; errors reject it.
    async fn receive_from_pump_from(
        &self,
        dir_idx: usize,
        input: WsPtr,
        max_size: usize,
    ) -> Result<ffi::WsMessage> {
        let result = input.receive(max_size).await;
        let mut dir = self.pipe.directions[dir_idx].borrow_mut();
        if let State::BlockedPumpFrom { busy, .. } = &mut dir.state {
            *busy = false;
            match &result {
                Ok(message) if message.kind == ffi::WsMessageKind::CLOSE => {
                    let State::BlockedPumpFrom { done, .. } =
                        std::mem::replace(&mut dir.state, State::Idle)
                    else {
                        unreachable!()
                    };
                    let _ = done.send(Ok(()));
                }
                Ok(_) => {}
                Err(e) => {
                    let State::BlockedPumpFrom { done, .. } =
                        std::mem::replace(&mut dir.state, State::Idle)
                    else {
                        unreachable!()
                    };
                    let _ = done.send(Err(e.clone()));
                }
            }
        }
        result
    }

    /// kj::WebSocket::pumpTo(): everything received on this end flows into `ws`.
    ///
    /// Returns a self-contained (`'static`) future: kj's pump promise holds the refcounted
    /// pipe, not the end object, so the promise may outlive the end (and be dropped after it,
    /// e.g. at event-loop teardown). Everything the future touches — including its drop-time
    /// deregistration — lives on the shared `Pipe`.
    pub fn pump_to(&self, ws: WsPtr) -> impl Future<Output = Result<()>> + 'static {
        Self::pump_to_impl(self.pipe.clone(), self.input, ws)
    }

    async fn pump_to_impl(pipe: Rc<Pipe>, input_idx: usize, ws: WsPtr) -> Result<()> {
        {
            let mut dir = pipe.directions[input_idx].borrow_mut();
            assert!(!dir.pumping_to, "can only call pumpTo() once at a time");
            dir.pumping_to = true;
            dir.dest_pumping_to = Some(ws);
        }
        let _cleanup = CallOnDrop(Some({
            let pipe = pipe.clone();
            move || {
                let mut dir = pipe.directions[input_idx].borrow_mut();
                dir.dest_pumping_to = None;
                dir.pumping_to = false;
            }
        }));
        select_biased! {
            result = Self::pump_to_no_abort(&pipe, input_idx, ws).fuse() => result,
            () = ws.when_aborted().fuse() => Err(disconnected(DEST_ABORTED_MSG)),
        }
    }

    async fn pump_to_no_abort(pipe: &Rc<Pipe>, dir_idx: usize, output: WsPtr) -> Result<()> {
        loop {
            let rx = {
                let mut dir = pipe.directions[dir_idx].borrow_mut();
                match &mut dir.state {
                    State::Aborted => return Err(disconnected(ABORTED_MSG)),
                    // Disconnected source: clean end of pump.
                    State::Disconnected => return Ok(()),
                    State::BlockedSend { busy, .. } => {
                        assert!(!*busy, "already pumping");
                        let State::BlockedSend { message, done, .. } =
                            std::mem::replace(&mut dir.state, State::Idle)
                        else {
                            unreachable!()
                        };
                        drop(dir);
                        // Deliver the parked message and keep pumping (kj recurses even for a
                        // parked Close -- preserve that quirk).
                        match message.deliver_to(output).await {
                            Ok(()) => {
                                let _ = done.send(Ok(()));
                                continue;
                            }
                            Err(e) => {
                                let _ = done.send(Err(e.clone()));
                                return Err(e);
                            }
                        }
                    }
                    State::BlockedPumpFrom { input, busy, .. } => {
                        assert!(!*busy, "{RECEIVE_IN_PROGRESS_MSG}");
                        *busy = true;
                        let input = *input;
                        drop(dir);
                        // Splice: source socket pumps straight into the destination; credit
                        // the destination's receivedByteCount delta (kj's rule 3).
                        let before = output.received_byte_count();
                        let result = input.pump_to(output).await;
                        let mut dir = pipe.directions[dir_idx].borrow_mut();
                        if let State::BlockedPumpFrom { .. } = dir.state {
                            let State::BlockedPumpFrom { done, .. } =
                                std::mem::replace(&mut dir.state, State::Idle)
                            else {
                                unreachable!()
                            };
                            let _ = done.send(result.clone().map(|()| ()));
                        }
                        dir.transferred += output.received_byte_count() - before;
                        return result;
                    }
                    State::BlockedReceive { .. } | State::BlockedPumpTo { .. } => {
                        return Err(failed(RECEIVE_IN_PROGRESS_MSG));
                    }
                    State::Idle => {
                        let (tx, rx) = oneshot::channel();
                        dir.generation += 1;
                        dir.state = State::BlockedPumpTo {
                            output,
                            done: tx,
                            busy: false,
                        };
                        rx
                    }
                }
            };
            let mut guard = ParkGuard {
                pipe: pipe.clone(),
                dir: dir_idx,
                generation: pipe.directions[dir_idx].borrow().generation,
                armed: true,
            };
            let result = match rx.await {
                Ok(result) => result,
                Err(_) => Err(disconnected(ABORTED_MSG)),
            };
            guard.armed = false;
            return result;
        }
    }

    /// kj::WebSocket::tryPumpFrom(): everything `ws` receives flows into this end.
    /// (Self-contained future; see `pump_to`.)
    pub fn pump_from(&self, ws: WsPtr) -> impl Future<Output = Result<()>> + 'static {
        Self::pump_from_impl(self.pipe.clone(), self.input, ws)
    }

    async fn pump_from_impl(pipe: Rc<Pipe>, input_idx: usize, ws: WsPtr) -> Result<()> {
        {
            let mut dir = pipe.directions[input_idx].borrow_mut();
            assert!(
                !dir.pumping_from,
                "can only call tryPumpFrom() once at a time"
            );
            dir.pumping_from = true;
            dir.dest_pumping_from = Some(ws);
        }
        let _cleanup = CallOnDrop(Some({
            let pipe = pipe.clone();
            move || {
                let mut dir = pipe.directions[input_idx].borrow_mut();
                dir.dest_pumping_from = None;
                dir.pumping_from = false;
            }
        }));
        Self::pump_from_inner(pipe, input_idx, ws).await
    }

    async fn pump_from_inner(pipe: Rc<Pipe>, input_idx: usize, input: WsPtr) -> Result<()> {
        // This runs on the OUT direction (kj: End::tryPumpFrom -> out->tryPumpFrom).
        let dir_idx = 1 - input_idx;
        let rx = {
            let mut dir = pipe.directions[dir_idx].borrow_mut();
            match &mut dir.state {
                State::Aborted => return Err(disconnected(ABORTED_MSG)),
                State::Disconnected => {
                    return Err(failed("can't tryPumpFrom() after disconnect()"));
                }
                State::BlockedReceive { max_size, busy, .. } => {
                    assert!(!*busy, "already pumping");
                    let max_size = *max_size;
                    *busy = true;
                    drop(dir);
                    // First message satisfies the parked receive; then keep pumping into the
                    // pipe (kj's BlockedReceive::tryPumpFrom).
                    let first = input.receive(max_size).await;
                    let mut dir = pipe.directions[dir_idx].borrow_mut();
                    match first {
                        Ok(message) => {
                            if let State::BlockedReceive { .. } = dir.state {
                                let State::BlockedReceive { done, .. } =
                                    std::mem::replace(&mut dir.state, State::Idle)
                                else {
                                    unreachable!()
                                };
                                let _ = done.send(Ok(message));
                            }
                            drop(dir);
                            return Box::pin(Self::pump_from_inner(pipe, input_idx, input)).await;
                        }
                        Err(e) => {
                            if let State::BlockedReceive { .. } = dir.state {
                                let State::BlockedReceive { done, .. } =
                                    std::mem::replace(&mut dir.state, State::Idle)
                                else {
                                    unreachable!()
                                };
                                let _ = done.send(Err(e.clone()));
                            }
                            return Err(e);
                        }
                    }
                }
                State::BlockedPumpTo { output, busy, .. } => {
                    assert!(!*busy, "{SEND_IN_PROGRESS_MSG}");
                    *busy = true;
                    let output = *output;
                    drop(dir);
                    // Splice pump-to-pump: the source pumps straight into the sink.
                    let result = input.pump_to(output).await;
                    let mut dir = pipe.directions[dir_idx].borrow_mut();
                    if let State::BlockedPumpTo { .. } = dir.state {
                        let State::BlockedPumpTo { done, .. } =
                            std::mem::replace(&mut dir.state, State::Idle)
                        else {
                            unreachable!()
                        };
                        let _ = done.send(result.clone());
                    }
                    return result;
                }
                State::BlockedSend { .. } | State::BlockedPumpFrom { .. } => {
                    return Err(failed(SEND_IN_PROGRESS_MSG));
                }
                State::Idle => {
                    let (tx, rx) = oneshot::channel();
                    dir.generation += 1;
                    dir.state = State::BlockedPumpFrom {
                        input,
                        done: tx,
                        busy: false,
                    };
                    rx
                }
            }
        };
        let mut guard = ParkGuard {
            pipe: pipe.clone(),
            dir: dir_idx,
            generation: pipe.directions[dir_idx].borrow().generation,
            armed: true,
        };
        let result = match rx.await {
            Ok(result) => result,
            Err(_) => Err(disconnected(ABORTED_MSG)),
        };
        guard.armed = false;
        result
    }

    pub fn sent_byte_count(&self) -> u64 {
        self.pipe.directions[self.output()].borrow().transferred
    }

    pub fn received_byte_count(&self) -> u64 {
        self.pipe.directions[self.input].borrow().transferred
    }

    /// The peer end's active pump target's preference (kj: reads the shared out-direction's
    /// destinationPumpingTo, then ...From).
    pub fn peer_preferred_extensions(&self, is_request_context: bool, out: &mut String) -> bool {
        let dir = self.pipe.directions[self.output()].borrow();
        for target in [dir.dest_pumping_to, dir.dest_pumping_from] {
            if let Some(ws) = target
                && let Some(preferred) = ws.preferred_extensions(is_request_context)
            {
                *out = preferred;
                return true;
            }
        }
        false
    }
}

/// Runs a closure on drop (pump-registration cleanup).
struct CallOnDrop<F: FnOnce()>(Option<F>);
impl<F: FnOnce()> Drop for CallOnDrop<F> {
    fn drop(&mut self) {
        if let Some(f) = self.0.take() {
            f();
        }
    }
}

/// `kj::WebSocket::SUGGESTED_MAX_MESSAGE_SIZE` (32 MiB), `receive()`'s default `maxSize` — the
/// size kj's `pumpWebSocketLoop` receives with.
const SUGGESTED_MAX_MESSAGE_SIZE: usize = 32 << 20;

/// `kj::WebSocket::pumpTo()`'s default fallback loop, a behavior-parity port of kj's
/// `pumpWebSocketLoop` (in the unlinked kj-http-impl; the workerd kj-http shim's
/// `WebSocket::pumpTo()` interface default calls this between two *foreign* sockets when the
/// destination's `tryPumpFrom()` declined): receive each message from `from` (kj's default
/// maxSize) and forward it to `to`; once a Close has passed through, the pump is complete. kj
/// doesn't know whether the read or the write side threw, so on any error the destination is
/// disconnect()ed (a redundant disconnect doesn't hurt) and the error propagates as the pump
/// result.
pub(crate) async fn default_pump(from: WsPtr, to: WsPtr) -> Result<()> {
    let result = async {
        loop {
            let message = from.receive(SUGGESTED_MAX_MESSAGE_SIZE).await?;
            match message.kind {
                // Text bytes pass through unvalidated, matching kj's non-validation of text
                // payloads.
                ffi::WsMessageKind::TEXT => to.send_text(&message.data).await?,
                ffi::WsMessageKind::BINARY => to.send_binary(&message.data).await?,
                _ => {
                    // Once a close has passed through, the pump is complete.
                    to.close(message.close_code, &message.data).await?;
                    return Ok(());
                }
            }
            // continue the loop
        }
    }
    .await;
    if result.is_err() {
        to.disconnect();
    }
    result
}
