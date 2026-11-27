// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Serving C++ calls from Rust.
//!
//! [`RustServer`] holds a server's generated dispatch; C++ wraps it in a
//! `capnp::Capability::Server` and calls [`dispatch_call`] for each call.

use std::cell::Cell;
use std::cell::RefCell;
use std::panic::AssertUnwindSafe;
use std::pin::Pin;
use std::rc::Rc;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;

use capnp::Error;
use capnp::any_pointer;
use capnp::capability::DispatchCallResult;
use capnp::capability::Params;
use capnp::capability::Promise;
use capnp::capability::Results;
use capnp::capability::Server;
use capnp::private::capability::ParamsHook;
use capnp::private::capability::PipelineHook;
use capnp::private::capability::RequestHook;
use capnp::private::capability::ResultsHook;
use kj_rs::KjOwn;

use crate::error::from_kj;
use crate::error::to_kj;
use crate::ffi::bridge;
use crate::message::CxxMessageReader;
use crate::message::RustMessage;

/// A server's generated dispatch, with its type erased.
trait ErasedServer {
    fn dispatch_call(
        &self,
        interface_id: u64,
        method_id: u16,
        params: Params<any_pointer::Owned>,
        results: Results<any_pointer::Owned>,
    ) -> DispatchCallResult;
}

impl<S: Server + Clone> ErasedServer for S {
    fn dispatch_call(
        &self,
        interface_id: u64,
        method_id: u16,
        params: Params<any_pointer::Owned>,
        results: Results<any_pointer::Owned>,
    ) -> DispatchCallResult {
        // The generated dispatch is an `Rc` of the server; cloning it is a reference count.
        self.clone()
            .dispatch_call(interface_id, method_id, params, results)
    }
}

/// What happens to a call when its caller cancels it.
///
/// A C++ server takes this from the `$Cxx.allowCancellation` annotation, which capnpc-c++ looks
/// for on the method, then the interface, then the file, and compiles into the server's dispatch.
/// capnpc-rust does not see the annotation, so a Rust server states it when it becomes a client,
/// and nothing checks the statement: capnpc-rust generates no schema nodes for interfaces, C++ has
/// no registry of compiled-in schemas by id, and the schemas capnpc-c++ compiles in have no file
/// nodes, so not even C++ could see a file-level annotation, which is where most of workerd's are.
/// It must match the annotations by hand.
#[derive(Clone, Copy, Debug)]
pub enum Cancellation {
    /// The call runs to completion whatever the caller does. KJ's default.
    RunToCompletion,
    /// Canceling the call drops the server's future. Matches `$Cxx.allowCancellation` on the file
    /// or interface, or on every method.
    Allow,
    /// Decided per method, for an interface whose methods differ, such as `EventDispatcher`:
    /// returns whether calls to the method with these ids may be canceled. The interface id
    /// distinguishes an interface's methods from those it inherits. If it panics, the call fails
    /// before the server sees it.
    PerMethod(fn(interface_id: u64, method_id: u16) -> bool),
}

impl Cancellation {
    fn allows(self, interface_id: u64, method_id: u16) -> bool {
        match self {
            Self::RunToCompletion => false,
            Self::Allow => true,
            Self::PerMethod(allows) => allows(interface_id, method_id),
        }
    }
}

/// The rest of a call that didn't complete when it was dispatched.
type PendingCall = Pin<Box<dyn Future<Output = Result<(), cxx::KjError>>>>;

/// A Rust server, as C++ holds it.
pub struct RustServer {
    server: Box<dyn ErasedServer>,
    cancellation: Cancellation,
    /// The call that [`dispatch_call`] last dispatched, if it didn't complete, for C++ to take
    /// with [`take_pending_call`] as soon as `dispatch_call` returns.
    pending: RefCell<Option<PendingCall>>,
}

impl RustServer {
    pub fn new<S: Server + Clone + 'static>(dispatch: S, cancellation: Cancellation) -> Self {
        Self {
            server: Box::new(dispatch),
            cancellation,
            pending: RefCell::new(None),
        }
    }

    pub fn allows_cancellation(&self, interface_id: u64, method_id: u16) -> bool {
        self.cancellation.allows(interface_id, method_id)
    }
}

/// Dispatches a call that C++ made to `server`, and runs the server's future until it first
/// waits, as a C++ server's method runs until its first `co_await`. Most servers complete then,
/// with their results in the C++ call (see [`KjResults`]), and their calls need no KJ promise of
/// a Rust future. A call that waits, or fails, leaves the rest of it for C++ to take with
/// [`take_pending_call`].
///
/// So a Rust server's code runs synchronously inside the C++ code that dispatches the call, as a
/// C++ server's does. That is usually a turn of KJ's event loop, but KJ also dispatches the next
/// queued call to a capability while it destroys the promise of a finished or canceled
/// `-> stream` call to it, which may happen inside another Rust future's poll: a server must not
/// hold a `RefCell` borrow that its own methods take across awaiting or dropping such a promise.
pub fn dispatch_call(
    server: &RustServer,
    interface_id: u64,
    method_id: u16,
    params: KjOwn<bridge::CxxMessage>,
    context: KjOwn<bridge::CallContextHook>,
) -> bridge::Dispatched {
    dispatch(
        server,
        interface_id,
        method_id,
        CxxMessageReader::new(params),
        context,
    )
}

/// The rest of the call that [`dispatch_call`] last left pending on `server`.
pub fn take_pending_call(
    server: &RustServer,
) -> impl Future<Output = Result<(), cxx::KjError>> + use<> {
    let pending = server.pending.take();
    async move {
        match pending {
            Some(call) => call.await,
            None => Err(to_kj(Error::failed(
                "capnp-kj: no call is pending on this server".to_owned(),
            ))),
        }
    }
}

fn dispatch(
    server: &RustServer,
    interface_id: u64,
    method_id: u16,
    params: CxxMessageReader,
    context: KjOwn<bridge::CallContextHook>,
) -> bridge::Dispatched {
    let delivery = Delivery::default();
    // If the server panics before the generated dispatch returns, whether the method is a
    // `-> stream` method is unknown, and it is taken not to be one: a failed `-> stream` call
    // fails every later call to the capability, which a panic in any other method must not do. So
    // a `-> stream` method that panics before it returns its future fails only its own call.
    let mut is_streaming = false;
    // Server code runs in the generated dispatch, which calls the method, and a method can panic
    // before it returns its future; in the first poll; and when a completed future is dropped. A
    // panic in any of them fails the call. The closure returns the future's result, or the future
    // if it waits.
    let first_poll = std::panic::catch_unwind(AssertUnwindSafe(|| {
        let dispatched = server.server.dispatch_call(
            interface_id,
            method_id,
            Params::new(Box::new(params)),
            Results::new(Box::new(KjResults {
                message: None,
                context,
                delivery: Rc::clone(&delivery),
            })),
        );
        is_streaming = dispatched.is_streaming;
        let mut promise = dispatched.promise;
        // The server's future need not wake this poll's waker, only that of its latest poll, so
        // this one can do nothing: kj-rs polls a pending future again, whether woken or not, once
        // C++ awaits the call's promise.
        match Pin::new(&mut promise).poll(&mut Context::from_waker(Waker::noop())) {
            Poll::Ready(result) => {
                // Dropped first, as awaiting it would, since the future may hold the results.
                drop(promise);
                Ok(result)
            }
            Poll::Pending => Err(promise),
        }
    }));
    let pending: PendingCall = match first_poll {
        Ok(Ok(result)) => match finish(result, &delivery) {
            Ok(()) => {
                return bridge::Dispatched {
                    complete: true,
                    is_streaming,
                };
            }
            // A failure goes in the call's promise, as a C++ coroutine's does, so that a failed
            // `-> stream` call fails the calls queued behind it.
            Err(error) => Box::pin(std::future::ready(Err(error))),
        },
        Ok(Err(promise)) => Box::pin(async move { finish(promise.await, &delivery) }),
        // A panic fails the call as kj-rs fails one in a later poll, and through the call's
        // promise too, so that a `-> stream` call that panics fails the calls queued behind it.
        Err(payload) => {
            let description = match cxx::panic_message(&*payload) {
                Some(message) => format!("panic in Rust future: {message}"),
                None => "panic in Rust future".to_owned(),
            };
            Box::pin(std::future::ready(Err(to_kj(Error::failed(description)))))
        }
    };
    let previous = server.pending.replace(Some(pending));
    debug_assert!(
        previous.is_none(),
        "C++ takes each pending call before dispatching another"
    );
    bridge::Dispatched {
        complete: false,
        is_streaming,
    }
}

/// The outcome of a call whose server's future completed with `result`.
fn finish(result: Result<(), Error>, delivery: &Delivery) -> Result<(), cxx::KjError> {
    result.map_err(to_kj)?;
    // Generated code drops the results before the server's future completes, unless the server
    // moved them somewhere that outlives the call.
    if Rc::strong_count(delivery) > 1 {
        return Err(to_kj(Error::failed(
            "capnp-kj: the server kept its results past the end of the call".to_owned(),
        )));
    }
    match delivery.take() {
        Some(error) => Err(to_kj(error)),
        None => Ok(()),
    }
}

/// Why a call's results didn't reach the C++ call, if they didn't; shared between the results and
/// the call's future, which fails with it.
type Delivery = Rc<Cell<Option<Error>>>;

/// A call's params, read in place.
impl ParamsHook for CxxMessageReader {
    fn get(&self) -> capnp::Result<any_pointer::Reader<'_>> {
        self.root()
    }
}

/// A call's results, built in Rust and copied into the C++ call when the server drops them, which
/// generated code does before the server's future completes. The message is created on the
/// server's first `get()`, so a call whose server writes no results builds none, and its caller
/// reads empty results, as from a C++ server that writes none.
struct KjResults {
    message: Option<RustMessage>,
    context: KjOwn<bridge::CallContextHook>,
    delivery: Delivery,
}

impl Drop for KjResults {
    fn drop(&mut self) {
        let Some(message) = self.message.take() else {
            return;
        };
        if let Err(error) = bridge::context_set_results(self.context.pin_mut(), &message) {
            self.delivery.set(Some(from_kj(&error)));
        }
    }
}

impl ResultsHook for KjResults {
    fn get(&mut self) -> capnp::Result<any_pointer::Builder<'_>> {
        self.message.get_or_insert_with(RustMessage::new).root()
    }

    fn set_pipeline(&mut self) -> capnp::Result<()> {
        Err(Error::unimplemented(
            "capnp-kj: set_pipeline() is not supported".to_owned(),
        ))
    }

    fn allow_cancellation(&self) {
        // KJ takes cancellation from `DispatchCallResult`; see `RustServerAdapter` in ffi.c++.
    }

    fn tail_call(self: Box<Self>, _request: Box<dyn RequestHook>) -> Promise<(), Error> {
        Promise::err(Error::unimplemented(
            "capnp-kj: tail calls are not supported".to_owned(),
        ))
    }

    fn direct_tail_call(
        self: Box<Self>,
        _request: Box<dyn RequestHook>,
    ) -> (Promise<(), Error>, Box<dyn PipelineHook>) {
        (
            Promise::err(Error::unimplemented(
                "capnp-kj: tail calls are not supported".to_owned(),
            )),
            Box::new(crate::client::broken_pipeline(
                "capnp-kj: tail calls are not supported",
            )),
        )
    }
}
