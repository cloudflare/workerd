// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `writeAsync()` and `endAsync()`: lol-html on a fiber, so that handlers can return promises.
//!
//! lol-html calls handlers synchronously, so a handler that waits for a promise has to keep
//! lol-html's stack while the event loop runs. A call runs lol-html on a fiber (see `fiber.h`),
//! whose handlers each run the dispatcher on the main stack, in the request's `IoContext`, and
//! wait there until the dispatcher's promise settles. The token stays lent until then, so an
//! async handler can use its token after an `await`, as with C++ `HTMLRewriter`.
//!
//! The fiber's [`FiberTask`] owns the rewriter while it runs, so that nothing it uses depends on
//! the handle staying alive, and holds no JavaScript values that it reaches without the isolate
//! lock: only the main stack, which has the lock, reaches those of [`AsyncCall`].

use std::cell::Cell;
use std::panic::AssertUnwindSafe;
use std::pin::Pin;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::TryLockError;

use futures::channel::oneshot;
use jsg::ToJS;
use jsg::ToJsTypedArray;
use jsg::v8;
use kj_rs::KjOwn;

use crate::ffi::bridge;
use crate::parser_error;
use crate::rewriter::Call;
use crate::rewriter::Dispatcher;
use crate::rewriter::HandlerKind;
use crate::rewriter::Rewriter;
use crate::rewriter::Shared;
use crate::rewriter::State;
use crate::rewriter::Stopped;
use crate::rewriter::lock_mutex;
use crate::tokens;

/// Why a call failed other than with a handler's exception.
enum Failure {
    /// lol-html failed, with this message.
    Parser(String),
    /// Waiting for a handler failed, as when the `IoContext` was aborted.
    Wait(jsg::Error),
    /// The fiber's Rust code panicked.
    Panicked,
}

/// What a call's fiber and the main stack share. The JavaScript values are reached only on the
/// main stack, with the isolate lock; dropping them without it is deferred to the next lock.
///
/// The fiber and the main stack run on one thread, but the reactions to a handler's promise,
/// which hold the call too, may run on any thread that holds the isolate lock.
pub struct AsyncCall {
    function: v8::Global<v8::Function>,
    frame: jsg::AsyncContextFrame,
    shared: Arc<Shared>,
    /// What the first handler that failed threw, or rejected its promise with.
    exception: Mutex<Option<v8::Global<v8::Value>>>,
    failure: Mutex<Option<Failure>>,
}

impl AsyncCall {
    /// A call that runs `function`, the dispatcher, in `frame`.
    pub fn new<'s>(
        lock: &mut jsg::Lock<'s>,
        function: v8::Local<'s, v8::Function>,
        frame: jsg::AsyncContextFrame,
        shared: &Arc<Shared>,
    ) -> Arc<Self> {
        Arc::new(Self {
            function: function.to_global(lock),
            frame,
            shared: Arc::clone(shared),
            exception: Mutex::new(None),
            failure: Mutex::new(None),
        })
    }

    fn failed(&self) -> bool {
        lock_mutex(&self.exception).is_some()
    }

    fn keep_exception<'s>(&self, lock: &mut jsg::Lock<'s>, exception: v8::Local<'s, v8::Value>) {
        let mut kept = lock_mutex(&self.exception);
        if kept.is_none() {
            *kept = Some(exception.to_global(lock));
        }
    }
}

/// What the rewriter's `write()` or `end()` gets.
enum Input {
    Write(Vec<u8>),
    End,
}

/// A `writeAsync()` or `endAsync()` call, which runs on a fiber.
pub struct FiberTask {
    call: Arc<AsyncCall>,
    state: Arc<Mutex<State>>,
    /// Taken by `end()`, which consumes it.
    rewriter: Option<Box<Rewriter>>,
    input: Input,
    ran: bool,
    /// Completed, or dropped, when the call is done.
    done: Option<oneshot::Sender<()>>,
}

impl FiberTask {
    /// Runs the call, then leaves the rewriter in `state`: open if the call wrote and succeeded,
    /// ended if it ended and succeeded, failed otherwise, or as `abort()` left it meanwhile.
    pub fn run(&mut self, wait: Pin<&mut bridge::FiberWait>) {
        self.ran = true;
        let call = Arc::clone(&self.call);
        let mut dispatcher = FiberDispatcher {
            wait,
            call: Arc::clone(&call),
            stopped: false,
        };
        let mut rewriter = self.rewriter.take();
        let input = std::mem::replace(&mut self.input, Input::End);
        call.shared.resume();
        let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
            call.shared
                .dispatcher
                .lend(&mut dispatcher, || match input {
                    Input::Write(chunk) => rewriter
                        .as_mut()
                        .map_or(Ok(()), |rewriter| rewriter.write(&chunk)),
                    Input::End => rewriter.take().map_or(Ok(()), |rewriter| rewriter.end()),
                })
        }));
        if let Err(error) = bridge::take_wait_error(dispatcher.wait.as_mut()) {
            lock_mutex(&call.failure).get_or_insert(Failure::Wait(error.into()));
        }
        let succeeded = match result {
            Ok(Ok(())) => !call.failed(),
            Ok(Err(error)) => {
                // A handler that stopped lol-html kept why, as did C++ if the wait failed.
                if !call.failed() && !dispatcher.stopped {
                    *lock_mutex(&call.failure) = Some(Failure::Parser(error.to_string()));
                }
                false
            }
            Err(_) => {
                *lock_mutex(&call.failure) = Some(Failure::Panicked);
                false
            }
        };
        let mut state = lock_mutex(&self.state);
        if matches!(*state, State::Running) {
            *state = match rewriter {
                Some(rewriter) if succeeded => State::Open(rewriter),
                None if succeeded => State::Ended,
                _ => State::Failed,
            };
        }
        drop(state);
        if let Some(done) = self.done.take() {
            // The receiver is gone if the IoContext was destroyed, and nothing waits then.
            let _ = done.send(());
        }
    }
}

impl Drop for FiberTask {
    /// A task dropped before it ran gives the rewriter back.
    fn drop(&mut self) {
        if self.ran {
            return;
        }
        let state = match self.state.try_lock() {
            Ok(state) => Some(state),
            Err(TryLockError::Poisoned(state)) => Some(state.into_inner()),
            Err(TryLockError::WouldBlock) => None,
        };
        if let Some(mut state) = state
            && matches!(*state, State::Running)
        {
            *state = self.rewriter.take().map_or(State::Failed, State::Open);
        }
    }
}

/// Runs handlers from the fiber on the main stack.
struct FiberDispatcher<'a> {
    wait: Pin<&'a mut bridge::FiberWait>,
    call: Arc<AsyncCall>,
    /// Whether a wait failed or was canceled, which C++ rethrows.
    stopped: bool,
}

impl Dispatcher for FiberDispatcher<'_> {
    fn dispatch(&mut self, call: Call<'_, '_, '_>) -> Result<(), Stopped> {
        let outcome = tokens::lend_slot(call.token, |slot| {
            let mut main = MainStackCall {
                call: Arc::clone(&self.call),
                kind: call.kind,
                id: call.id,
                slot: Some(slot),
                output: call.output,
            };
            bridge::run_on_main_stack(self.wait.as_mut(), &mut main)
        });
        if outcome != bridge::WaitOutcome::Continue {
            self.stopped = true;
            return Err(Stopped);
        }
        if self.call.failed() {
            return Err(Stopped);
        }
        Ok(())
    }
}

/// A handler that the fiber runs on the main stack.
pub struct MainStackCall {
    call: Arc<AsyncCall>,
    kind: HandlerKind,
    id: u32,
    /// Taken by the handle the call creates.
    slot: Option<tokens::Slot>,
    output: Vec<u8>,
}

impl MainStackCall {
    /// Calls the dispatcher in the async context frame of `transform()`, and settles `settler`
    /// when it is done: at once, unless it returns a promise, and when the promise settles
    /// otherwise. An exception, the handler's or one in calling it, is kept for the call to fail
    /// with. Only a termination escapes, and drops `settler`.
    pub fn run(
        &mut self,
        lock: &mut jsg::Lock<'_>,
        settler: KjOwn<bridge::Settler>,
    ) -> bridge::MainStackOutcome {
        let call = Arc::clone(&self.call);
        let settler = Cell::new(Some(settler));
        let slot = self.slot.take();
        let output = std::mem::take(&mut self.output);
        let (kind, id) = (self.kind, self.id);
        let result = call.frame.enter(lock, |lock| {
            lock.try_catch(|lock| {
                let slot = slot
                    .ok_or_else(|| jsg::Error::new_internal_error("the handler call ran twice"))?;
                let token = tokens::handle(lock, &call.shared, slot)?;
                let output = if output.is_empty() {
                    lock.undefined()
                } else {
                    output.to_js_typed_array(lock).into()
                };
                let kind = (kind as u8).to_js(lock)?;
                let id = id.to_js(lock)?;
                let function = call.function.as_local(lock);
                let value = function.call(lock, None, &[kind, id, token, output])?;
                if let Ok(promise) = v8::Local::<v8::Promise>::try_from(value) {
                    let reaction = Reaction {
                        settler: Cell::new(settler.take()),
                        call: Arc::clone(&call),
                    };
                    promise.then_or_catch(
                        lock,
                        reaction,
                        |lock, reaction, _value| {
                            reaction.settle();
                            Ok(lock.undefined())
                        },
                        |lock, reaction, exception| {
                            reaction.call.keep_exception(lock, exception);
                            reaction.settle();
                            Ok(lock.undefined())
                        },
                    )?;
                }
                Ok(())
            })
        });
        let exception = match result {
            Ok(Ok(())) => None,
            Ok(Err(exception)) => Some(exception),
            Err(error) if !error.is_pending() => match lock.exception(&error) {
                Ok(exception) => Some(exception),
                Err(_) => return bridge::MainStackOutcome::Terminated,
            },
            Err(_) => return bridge::MainStackOutcome::Terminated,
        };
        if let Some(exception) = exception {
            call.keep_exception(lock, exception);
        }
        if let Some(settler) = settler.take() {
            bridge::settle(settler);
        }
        bridge::MainStackOutcome::Dispatched
    }
}

/// What a handler's promise settles. Dropped unsettled, if V8 drops the reactions, it ends the
/// fiber's wait with an error. The reactions may run, and drop it, on any thread that holds the
/// isolate lock, which is why the settler's fulfiller is a cross-thread one.
struct Reaction {
    settler: Cell<Option<KjOwn<bridge::Settler>>>,
    call: Arc<AsyncCall>,
}

impl Reaction {
    fn settle(&self) {
        if let Some(settler) = self.settler.take() {
            bridge::settle(settler);
        }
    }
}

impl jsg::Traced for Reaction {
    // The JavaScript values `call` holds are strong handles, not traced ones.
    fn trace(&self, _visitor: &mut jsg::GcVisitor) {}
}

/// Starts `call` for `rewriter`, which `state` says is running: a `writeAsync()` if `chunk` is
/// `Some`, an `endAsync()` otherwise. Returns a promise for the output.
pub fn start<'s>(
    lock: &mut jsg::Lock<'s>,
    context: &io_context::IoContext<'s>,
    call: Arc<AsyncCall>,
    state: &Arc<Mutex<State>>,
    rewriter: Box<Rewriter>,
    chunk: Option<Vec<u8>>,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    let (done, receiver) = oneshot::channel();
    let task = FiberTask {
        call: Arc::clone(&call),
        state: Arc::clone(state),
        rewriter: Some(rewriter),
        input: chunk.map_or(Input::End, Input::Write),
        ran: false,
        done: Some(done),
    };
    bridge::start_fiber(Box::new(task))?;
    // The receiver fails only if the task was dropped before it ran, when the IoContext is
    // destroyed, and then nothing waits for it.
    let done = async move { receiver.await.is_ok() };
    context.await_io(lock, done, move |lock, ran| {
        let exception = lock_mutex(&call.exception).take();
        if let Some(exception) = exception {
            let exception = exception.as_local(lock);
            return Ok(v8::Promise::rejected(lock, exception)?.into());
        }
        if !ran {
            return Err(jsg::Error::new_internal_error(
                "the HTMLRewriter fiber never ran",
            ));
        }
        let failure = lock_mutex(&call.failure).take();
        match failure {
            Some(Failure::Parser(message)) => return Err(parser_error(message)),
            Some(Failure::Wait(error)) => return Err(error),
            Some(Failure::Panicked) => {
                return Err(jsg::Error::new_internal_error(
                    "the HTMLRewriter fiber panicked",
                ));
            }
            None => {}
        }
        Ok(call.shared.take_output().to_js_typed_array(lock).into())
    })
}
