// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Serving C++ calls from Rust.
//!
//! [`RustServer`] holds a server's generated dispatch; C++ wraps it in a
//! `capnp::Capability::Server` and calls [`RustServer::dispatch`] for each call.

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
use futures::channel::oneshot;
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
}

impl RustServer {
    pub fn new<S: Server + Clone + 'static>(dispatch: S, cancellation: Cancellation) -> Self {
        Self {
            server: Box::new(dispatch),
            cancellation,
        }
    }

    pub fn allows_cancellation(&self, interface_id: u64, method_id: u16) -> bool {
        self.cancellation.allows(interface_id, method_id)
    }

    #[expect(
        clippy::unnecessary_box_returns,
        reason = "C++ holds the call as a rust::Box"
    )]
    pub fn dispatch(
        &self,
        interface_id: u64,
        method_id: u16,
        params: KjOwn<bridge::CxxMessage>,
    ) -> Box<RustCall> {
        let (sender, results) = oneshot::channel();
        let dispatched = self.server.dispatch_call(
            interface_id,
            method_id,
            Params::new(Box::new(KjParams {
                message: CxxMessageReader::new(params),
            })),
            Results::new(Box::new(KjResults {
                message: Some(RustMessage::new()),
                sender: Some(sender),
            })),
        );
        Box::new(RustCall {
            is_streaming: dispatched.is_streaming,
            promise: dispatched.promise,
            results,
        })
    }
}

/// A call C++ made to a Rust server.
pub struct RustCall {
    is_streaming: bool,
    promise: Promise<(), Error>,
    results: oneshot::Receiver<RustMessage>,
}

impl RustCall {
    pub fn is_streaming(&self) -> bool {
        self.is_streaming
    }
}

/// Runs a call to completion and returns its results for C++ to copy.
pub async fn run_call(call: Box<RustCall>) -> Result<Box<RustMessage>, cxx::KjError> {
    let RustCall {
        promise, results, ..
    } = *call;
    promise.await.map_err(to_kj)?;
    // The results are sent when the server drops them, which it has done by the time a
    // well-behaved server's future completes.
    let results = results.await.map_err(|_| {
        to_kj(Error::failed(
            "server completed the call without its results".to_owned(),
        ))
    })?;
    Ok(Box::new(results))
}

/// A call's params, read in place from the C++ message.
struct KjParams {
    message: CxxMessageReader,
}

impl ParamsHook for KjParams {
    fn get(&self) -> capnp::Result<any_pointer::Reader<'_>> {
        self.message.root()
    }
}

/// A call's results, built in Rust and sent back to C++ when the server drops them.
struct KjResults {
    message: Option<RustMessage>,
    sender: Option<oneshot::Sender<RustMessage>>,
}

impl Drop for KjResults {
    fn drop(&mut self) {
        if let (Some(message), Some(sender)) = (self.message.take(), self.sender.take()) {
            // Fails only if C++ canceled the call, in which case nobody wants the results.
            let _ = sender.send(message);
        }
    }
}

impl ResultsHook for KjResults {
    fn get(&mut self) -> capnp::Result<any_pointer::Builder<'_>> {
        match self.message.as_mut() {
            Some(message) => message.root(),
            None => Err(Error::failed("results already sent".to_owned())),
        }
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
