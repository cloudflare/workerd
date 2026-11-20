// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Serving C++ calls from Rust.
//!
//! [`RustServer`] holds a server's generated dispatch; C++ wraps it in a
//! `capnp::Capability::Server` and calls [`RustServer::dispatch`] for each call.

use std::cell::Cell;

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
/// C++ servers take this from the `$Cxx.allowCancellation` annotation, which capnpc-rust does not
/// see, so a Rust server states it when it becomes a client. Neither side can check the statement
/// against the schema: capnpc-rust generates no schema for interfaces, and C++ has no registry of
/// compiled-in schemas by id. It must match the annotations by hand.
#[derive(Clone, Copy, Debug)]
pub enum Cancellation {
    /// The call runs to completion whatever the caller does. KJ's default.
    RunToCompletion,
    /// Canceling the call drops the server's future. Matches `$Cxx.allowCancellation` on the file
    /// or interface.
    Allow,
    /// Decided per method, for an interface whose methods differ, such as `EventDispatcher`:
    /// returns whether calls to the method with these ids may be canceled. The interface id
    /// distinguishes an interface's methods from those it inherits.
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

/// A Rust server, as C++ holds it.
pub struct RustServer {
    server: Box<dyn ErasedServer>,
    cancellation: Cancellation,
    /// Whether the call that [`dispatch_call`] last dispatched is a `-> stream` call, for C++ to
    /// read once `dispatch_call` returns. Dispatching only creates the server's future, so no
    /// other call is dispatched in between.
    last_is_streaming: Cell<bool>,
}

impl RustServer {
    pub fn new<S: Server + Clone + 'static>(dispatch: S, cancellation: Cancellation) -> Self {
        Self {
            server: Box::new(dispatch),
            cancellation,
            last_is_streaming: Cell::new(false),
        }
    }

    pub fn allows_cancellation(&self, interface_id: u64, method_id: u16) -> bool {
        self.cancellation.allows(interface_id, method_id)
    }

    pub fn last_is_streaming(&self) -> bool {
        self.last_is_streaming.get()
    }
}

/// Dispatches a call that C++ made to `server`, and returns the call's future. The dispatch
/// happens now, when C++ calls this, rather than when the future is first polled, so that C++
/// can read [`RustServer::last_is_streaming`] right away; the call needs no state on the heap
/// besides the future.
///
/// The call's results are in the C++ call by the time the future completes; see [`KjResults`].
pub fn dispatch_call(
    server: &RustServer,
    interface_id: u64,
    method_id: u16,
    params: KjOwn<bridge::CxxMessage>,
    context: KjOwn<bridge::CallContextHook>,
) -> impl Future<Output = Result<(), cxx::KjError>> + use<> {
    dispatch(
        server,
        interface_id,
        method_id,
        KjParams::Cxx(CxxMessageReader::new(params)),
        ResultsContext::Cxx(context),
    )
}

/// Like [`dispatch_call`], for a call from Rust: the server reads the caller's message, and the
/// caller reads the server's.
pub fn dispatch_rust_call(
    server: &RustServer,
    interface_id: u64,
    method_id: u16,
    params: Box<RustMessage>,
    context: KjOwn<bridge::CallContextHook>,
) -> impl Future<Output = Result<(), cxx::KjError>> + use<> {
    dispatch(
        server,
        interface_id,
        method_id,
        KjParams::Rust(params),
        ResultsContext::Rust(context),
    )
}

fn dispatch(
    server: &RustServer,
    interface_id: u64,
    method_id: u16,
    params: KjParams,
    context: ResultsContext,
) -> impl Future<Output = Result<(), cxx::KjError>> + use<> {
    let dispatched = server.server.dispatch_call(
        interface_id,
        method_id,
        Params::new(Box::new(params)),
        Results::new(Box::new(KjResults {
            message: None,
            context,
        })),
    );
    server.last_is_streaming.set(dispatched.is_streaming);
    let promise = dispatched.promise;
    async move { promise.await.map_err(to_kj) }
}

/// A call's params, read in place.
enum KjParams {
    /// From a C++ caller.
    Cxx(CxxMessageReader),
    /// From a Rust caller.
    Rust(Box<RustMessage>),
}

impl ParamsHook for KjParams {
    fn get(&self) -> capnp::Result<any_pointer::Reader<'_>> {
        match self {
            Self::Cxx(message) => message.root(),
            Self::Rust(message) => message.root_as_reader(),
        }
    }
}

/// The C++ call that a Rust server's results go to.
enum ResultsContext {
    /// Any call: the results are copied into it.
    Cxx(KjOwn<bridge::CallContextHook>),
    /// A call from Rust: the results are handed over.
    Rust(KjOwn<bridge::CallContextHook>),
}

/// A call's results, built in Rust and handed to the C++ call when the server drops them, which
/// generated code does before the server's future completes. The message is created on the
/// server's first `get()`, so a call whose server writes no results builds none, and its caller
/// reads empty results, as from a C++ server that writes none.
struct KjResults {
    message: Option<RustMessage>,
    context: ResultsContext,
}

impl Drop for KjResults {
    fn drop(&mut self) {
        let Some(mut message) = self.message.take() else {
            return;
        };
        // Neither fails unless the bridge is broken: copying fails only on a malformed segment,
        // which capnp-rust does not produce, and handing over only if C++ set the results
        // first, which nothing does for a Rust server's call.
        let _ = match &mut self.context {
            ResultsContext::Cxx(context) => {
                bridge::context_set_results(context.pin_mut(), &mut message)
            }
            ResultsContext::Rust(context) => {
                bridge::context_set_rust_results(context.pin_mut(), Box::new(message))
            }
        };
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
